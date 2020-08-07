/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright (c) 2014-2019, Regents of the University of California.
 *
 * This file is part of NDN repo-ng (Next generation of NDN repository).
 * See AUTHORS.md for complete list of repo-ng authors and contributors.
 *
 * repo-ng is free software: you can redistribute it and/or modify it under the terms
 * of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * repo-ng is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * repo-ng, e.g., in COPYING.md file.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "../src/repo-command-parameter.hpp"
#include "../src/repo-command-response.hpp"

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/command-interest-signer.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/util/scheduler.hpp>

#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include <fstream>
#include <iostream>
#include <string>

#include <boost/asio.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/iostreams/operations.hpp>
#include <boost/iostreams/read.hpp>

#include "../src/manifest/manifest.hpp"

namespace repo {

using namespace ndn::time;

using std::shared_ptr;
using std::make_shared;
using std::bind;

static const uint64_t DEFAULT_BLOCK_SIZE = 1000;
static const uint64_t DEFAULT_INTEREST_LIFETIME = 4000;
static const uint64_t DEFAULT_FRESHNESS_PERIOD = 10000;
static const uint64_t DEFAULT_CHECK_PERIOD = 1000;
static const size_t PRE_SIGN_DATA_COUNT = 11;

class NdnPutFile : boost::noncopyable
{
public:
  class Error : public std::runtime_error
  {
  public:
    using std::runtime_error::runtime_error;
  };

  NdnPutFile()
    : isSingle(false)
    , useDigestSha256(false)
    , useDigestBlake2s(false)
    , freshnessPeriod(DEFAULT_FRESHNESS_PERIOD)
    , interestLifetime(DEFAULT_INTEREST_LIFETIME)
    , hasTimeout(false)
    , timeout(0)
    , blockSize(DEFAULT_BLOCK_SIZE)
    , insertStream(nullptr)
    , isVerbose(false)
    , m_scheduler(m_face.getIoService())
    , m_timestampVersion(toUnixTimestamp(system_clock::now()).count())
    , m_processId(0)
    , m_checkPeriod(DEFAULT_CHECK_PERIOD)
    , m_currentSegmentNo(0)
    , m_isFinished(false)
    , m_cmdSigner(m_keyChain)
  {
  }

  void
  run();

private:
  void
  prepareNextData(uint64_t referenceSegmentNo);

  void
  startInsertCommand();

  void
  onInsertCommandResponse(const ndn::Interest& interest, const ndn::Data& data);

  void
  onInsertCommandTimeout(const ndn::Interest& interest);

  void
  onInterest(const ndn::Name& prefix, const ndn::Interest& interest);

  void
  sendManifest(const ndn::Name& prefix, const ndn::Interest& interest);

  void
  onRegisterSuccess(const ndn::Name& prefix);

  void
  onRegisterFailed(const ndn::Name& prefix, const std::string& reason);

  void
  stopProcess();

  void
  signData(ndn::Data& data);

  void
  startCheckCommand();

  void
  onCheckCommandResponse(const ndn::Interest& interest, const ndn::Data& data);

  void
  onCheckCommandTimeout(const ndn::Interest& interest);

  ndn::Interest
  generateCommandInterest(const ndn::Name& commandPrefix, const std::string& command,
                          const RepoCommandParameter& commandParameter);

public:
  bool isSingle;
  bool useDigestSha256;
  bool useDigestBlake2s;
  std::string identityForData;
  std::string identityForCommand;
  milliseconds freshnessPeriod;
  milliseconds interestLifetime;
  bool hasTimeout;
  milliseconds timeout;
  size_t blockSize;
  ndn::Name repoPrefix;
  ndn::Name ndnName;
  std::istream* insertStream;
  bool isVerbose;

private:
  ndn::Face m_face;
  ndn::Scheduler m_scheduler;
  ndn::KeyChain m_keyChain;
  uint64_t m_timestampVersion;
  uint64_t m_processId;
  milliseconds m_checkPeriod;
  size_t m_currentSegmentNo;
  bool m_isFinished;
  ndn::Name m_dataPrefix;

  size_t m_bytes;

  using DataContainer = std::map<uint64_t, shared_ptr<ndn::Data>>;
  DataContainer m_data;
  ndn::security::CommandInterestSigner m_cmdSigner;
};

void
NdnPutFile::prepareNextData(uint64_t referenceSegmentNo)
{
  // make sure m_data has [referenceSegmentNo, referenceSegmentNo + PRE_SIGN_DATA_COUNT] Data
  if (m_isFinished)
    return;

  size_t nDataToPrepare = PRE_SIGN_DATA_COUNT;

  if (!m_data.empty()) {
    uint64_t maxSegmentNo = m_data.rbegin()->first;

    if (maxSegmentNo - referenceSegmentNo >= nDataToPrepare) {
      // nothing to prepare
      return;
    }

    nDataToPrepare -= maxSegmentNo - referenceSegmentNo;
  }

  for (size_t i = 0; i < nDataToPrepare && !m_isFinished; ++i) {
    uint8_t *buffer = new uint8_t[blockSize];
    auto readSize = boost::iostreams::read(*insertStream,
                                           reinterpret_cast<char*>(buffer), blockSize);
    if (readSize <= 0) {
      BOOST_THROW_EXCEPTION(Error("Error reading from the input stream"));
    }

    auto data = make_shared<ndn::Data>(Name(m_dataPrefix).appendSegment(m_currentSegmentNo));

    if (insertStream->peek() == std::istream::traits_type::eof()) {
      data->setFinalBlock(ndn::name::Component::fromSegment(m_currentSegmentNo));
      m_isFinished = true;
    }

    data->setContent(buffer, readSize);
    data->setFreshnessPeriod(freshnessPeriod);
    signData(*data);

    m_data.insert(std::make_pair(m_currentSegmentNo, data));

    ++m_currentSegmentNo;
    delete[] buffer;
  }
}

void
NdnPutFile::run()
{
  m_dataPrefix = ndnName;

  insertStream->seekg(0, std::ios::beg);
  auto beginPos = insertStream->tellg();
  insertStream->seekg(0, std::ios::end);
  m_bytes = insertStream->tellg() - beginPos;
  insertStream->seekg(0, std::ios::beg);

  if (isVerbose)
    std::cerr << "setInterestFilter for " << m_dataPrefix << std::endl;
  m_face.setInterestFilter(m_dataPrefix,
                           bind(&NdnPutFile::onInterest, this, _1, _2),
                           bind(&NdnPutFile::onRegisterSuccess, this, _1),
                           bind(&NdnPutFile::onRegisterFailed, this, _1, _2));

  if (hasTimeout)
    m_scheduler.schedule(timeout, [this] { stopProcess(); });

  m_face.processEvents();
}

void
NdnPutFile::onRegisterSuccess(const Name& prefix)
{
  startInsertCommand();
}

void
NdnPutFile::startInsertCommand()
{
  RepoCommandParameter parameters;
  parameters.setName(m_dataPrefix);

  ndn::Interest commandInterest = generateCommandInterest(repoPrefix, "insert", parameters);
  m_face.expressInterest(commandInterest,
                         bind(&NdnPutFile::onInsertCommandResponse, this, _1, _2),
                         bind(&NdnPutFile::onInsertCommandTimeout, this, _1), // Nack
                         bind(&NdnPutFile::onInsertCommandTimeout, this, _1));
}

void
NdnPutFile::onInsertCommandResponse(const ndn::Interest& interest, const ndn::Data& data)
{
  RepoCommandResponse response(data.getContent().blockFromValue());
  auto statusCode = response.getCode();
  if (statusCode >= 400) {
    std::cout << "Reason: " << response.getText() << std::endl;
    BOOST_THROW_EXCEPTION(Error("insert command failed with code " +
                                boost::lexical_cast<std::string>(statusCode)));
  }
  m_processId = response.getProcessId();

  m_scheduler.schedule(m_checkPeriod, [this] { startCheckCommand(); });
}

void
NdnPutFile::onInsertCommandTimeout(const ndn::Interest& interest)
{
  BOOST_THROW_EXCEPTION(Error("command response timeout"));
}

void
NdnPutFile::onInterest(const ndn::Name& prefix, const ndn::Interest& interest)
{
  if (interest.getName().size() == prefix.size()) {
    sendManifest(prefix, interest);
    return;
  }

  uint64_t segmentNo;
  try {
    ndn::Name::Component segmentComponent = interest.getName().get(prefix.size());
    segmentNo = segmentComponent.toSegment();
  }
  catch (const tlv::Error& e) {
    if (isVerbose) {
      std::cerr << "Error processing incoming interest " << interest << ": "
                << e.what() << std::endl;
    }
    return;
  }

  prepareNextData(segmentNo);

  DataContainer::iterator item = m_data.find(segmentNo);
  if (item == m_data.end()) {
    if (isVerbose) {
      std::cerr << "Requested segment [" << segmentNo << "] does not exist" << std::endl;
    }
    return;
  }

  if (m_isFinished) {
    uint64_t final = m_currentSegmentNo - 1;
    item->second->setFinalBlock(ndn::name::Component::fromSegment(final));
  }
  m_face.put(*item->second);
}

void
NdnPutFile::sendManifest(const ndn::Name& prefix, const ndn::Interest& interest)
{
  BOOST_ASSERT(prefix == m_dataPrefix);

  if (prefix != interest.getName()) {
    if (isVerbose) {
      std::cerr << "Received unexpected interest " << interest << std::endl;
    }
    return;
  }

  ndn::Data data(interest.getName());
  auto blockCount = m_bytes / blockSize + (m_bytes % blockSize != 0);

  Manifest manifest(interest.getName().toUri(), 0, blockCount - 1);
  std::string json = manifest.toInfoJson();
  data.setContent((uint8_t*) json.data(), (size_t) json.size());
  data.setFreshnessPeriod(freshnessPeriod);
  signData(data);

  m_face.put(data);
}

void
NdnPutFile::onRegisterFailed(const ndn::Name& prefix, const std::string& reason)
{
  BOOST_THROW_EXCEPTION(Error("onRegisterFailed: " + reason));
}

void
NdnPutFile::stopProcess()
{
  m_face.getIoService().stop();
}

void
NdnPutFile::signData(ndn::Data& data)
{
  if (useDigestSha256) {
    m_keyChain.sign(data, ndn::signingWithSha256());
  }
  else if (useDigestBlake2s) {
    m_keyChain.sign(data, ndn::signingWithBlake2s());
  }
  else if (identityForData.empty())
    m_keyChain.sign(data);
  else {
    m_keyChain.sign(data, ndn::signingByIdentity(identityForData));
  }
}

void
NdnPutFile::startCheckCommand()
{
  auto parameter = RepoCommandParameter();
  parameter.setName(ndnName);
  ndn::Interest checkInterest = generateCommandInterest(repoPrefix, "insert check",
                                                        parameter
                                                          .setProcessId(m_processId));
  m_face.expressInterest(checkInterest,
                         bind(&NdnPutFile::onCheckCommandResponse, this, _1, _2),
                         bind(&NdnPutFile::onCheckCommandTimeout, this, _1), // Nack
                         bind(&NdnPutFile::onCheckCommandTimeout, this, _1));
}

void
NdnPutFile::onCheckCommandResponse(const ndn::Interest& interest, const ndn::Data& data)
{
  RepoCommandResponse response(data.getContent().blockFromValue());
  auto statusCode = response.getCode();
  if (statusCode >= 400) {
    BOOST_THROW_EXCEPTION(Error("Insert check command failed with code: " +
                                boost::lexical_cast<std::string>(statusCode)));
  }

  if (m_isFinished) {
    uint64_t insertCount = response.getInsertNum();

    // Technically, the check should not infer, but directly has signal from repo that
    // write operation has been finished

    if (insertCount == m_currentSegmentNo) {
      m_face.getIoService().stop();
      return;
    }
  }

  m_scheduler.schedule(m_checkPeriod, [this] { startCheckCommand(); });
}

void
NdnPutFile::onCheckCommandTimeout(const ndn::Interest& interest)
{
  BOOST_THROW_EXCEPTION(Error("check response timeout"));
}

ndn::Interest
NdnPutFile::generateCommandInterest(const ndn::Name& commandPrefix, const std::string& command,
                                    const RepoCommandParameter& commandParameter)
{
  Name cmd = commandPrefix;
  cmd
    .append(command)
    .append(commandParameter.wireEncode());
  ndn::Interest interest;

  if (identityForCommand.empty())
    interest = m_cmdSigner.makeCommandInterest(cmd);
  else {
    interest = m_cmdSigner.makeCommandInterest(cmd, ndn::signingByIdentity(identityForCommand));
  }

  interest.setInterestLifetime(interestLifetime);
  return interest;
}

static void
usage(const char* programName)
{
  std::cerr << "Usage: "
            << programName << " [-u] [-D] [-d] [-s block size] [-i identity] [-I identity] [-x freshness]"
                              " [-l lifetime] [-w timeout] repo-prefix ndn-name filename\n"
            << "\n"
            << "Write a file into a repo.\n"
            << "\n"
            << "  -D: use DigestSha256 signing method instead of SignatureSha256WithRsa\n"
            << "  -B: use DigestBlake2s signing method instead of SignatureSha256WithRsa\n"
            << "  -i: specify identity used for signing Data\n"
            << "  -I: specify identity used for signing commands\n"
            << "  -x: FreshnessPeriod in milliseconds\n"
            << "  -l: InterestLifetime in milliseconds for each command\n"
            << "  -w: timeout in milliseconds for whole process (default unlimited)\n"
            << "  -s: block size (default 1000)\n"
            << "  -v: be verbose\n"
            << "  repo-prefix: repo command prefix\n"
            << "  ndn-name: NDN Name prefix for written Data\n"
            << "  filename: local file name; \"-\" reads from stdin\n"
            << std::endl;
}

static int
main(int argc, char** argv)
{
  NdnPutFile ndnPutFile;

  int opt;
  while ((opt = getopt(argc, argv, "hDBi:I:x:l:w:s:v")) != -1) {
    switch (opt) {
    case 'h':
      usage(argv[0]);
      return 0;
    case 'D':
      ndnPutFile.useDigestSha256 = true;
      break;
    case 'B':
      ndnPutFile.useDigestBlake2s = true;
      break;
    case 'i':
      ndnPutFile.identityForData = std::string(optarg);
      break;
    case 'I':
      ndnPutFile.identityForCommand = std::string(optarg);
      break;
    case 'x':
      try {
        ndnPutFile.freshnessPeriod = milliseconds(boost::lexical_cast<uint64_t>(optarg));
      }
      catch (const boost::bad_lexical_cast&) {
        std::cerr << "-x option should be an integer" << std::endl;;
        return 2;
      }
      break;
    case 'l':
      try {
        ndnPutFile.interestLifetime = milliseconds(boost::lexical_cast<uint64_t>(optarg));
      }
      catch (const boost::bad_lexical_cast&) {
        std::cerr << "-l option should be an integer" << std::endl;;
        return 2;
      }
      break;
    case 'w':
      ndnPutFile.hasTimeout = true;
      try {
        ndnPutFile.timeout = milliseconds(boost::lexical_cast<uint64_t>(optarg));
      }
      catch (const boost::bad_lexical_cast&) {
        std::cerr << "-w option should be an integer" << std::endl;;
        return 2;
      }
      break;
    case 's':
      try {
        ndnPutFile.blockSize = boost::lexical_cast<uint64_t>(optarg);
      }
      catch (const boost::bad_lexical_cast&) {
        std::cerr << "-s option should be an integer.";
        return 1;
      }
      break;
    case 'v':
      ndnPutFile.isVerbose = true;
      break;
    default:
      usage(argv[0]);
      return 2;
    }

    if (ndnPutFile.useDigestSha256 && ndnPutFile.useDigestBlake2s) {
      std::cerr << "-D option cannot use with -B" << std::endl;
      return 1;
    }
  }

  if (argc != optind + 3) {
    usage(argv[0]);
    return 2;
  }

  argc -= optind;
  argv += optind;

  ndnPutFile.repoPrefix = Name(argv[0]);
  ndnPutFile.ndnName = Name(argv[1]);
  if (strcmp(argv[2], "-") == 0) {
    ndnPutFile.insertStream = &std::cin;
    ndnPutFile.run();
  }
  else {
    std::ifstream inputFileStream(argv[2], std::ios::in | std::ios::binary);
    if (!inputFileStream.is_open()) {
      std::cerr << "ERROR: cannot open " << argv[2] << std::endl;
      return 2;
    }

    ndnPutFile.insertStream = &inputFileStream;
    ndnPutFile.run();
  }

  // ndnPutFile MUST NOT be used anymore because .insertStream is a dangling pointer

  return 0;
}

} // namespace repo

int
main(int argc, char** argv)
{
  try {
    return repo::main(argc, argv);
  }
  catch (const std::exception& e) {
    std::cerr << "ERROR: " << e.what() << std::endl;
    return 1;
  }
}
