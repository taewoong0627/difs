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
#include "../src/util.hpp"

#define _GNU_SOURCE  
#include <sched.h>
#include <time.h>
#include <stdio.h>
#include <sys/stat.h>

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


char* file_name;
clock_t start, end;
double time_result;
bool flag = false;

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
  prepareHashes();
  
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
  signFirstData(ndn::Data& data);

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
  //bool start_flag = false;
  bool isSingle;
  bool useDigestSha256;
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
  std::list<std::array<uint8_t,util::HASH_SIZE>> hashes;

  size_t m_bytes;
  size_t m_firstSize;

  using DataContainer = std::map<uint64_t, shared_ptr<ndn::Data>>;
  DataContainer m_data;
  ndn::security::CommandInterestSigner m_cmdSigner;
};

void
NdnPutFile::prepareHashes()
{
  int dataSize = blockSize - util::HASH_SIZE;
  std::array<uint8_t,util::HASH_SIZE> hash;
  std::array<uint8_t,util::HASH_SIZE> prevHash;
  uint8_t *buffer = new uint8_t[blockSize];

  int position;
  for (position = dataSize; position < (int)m_bytes ; position += dataSize) {
    if (!hashes.empty()) {
      prevHash = hashes.front();
    }
    memcpy(buffer, prevHash.data(), util::HASH_SIZE);
    // This part is to read from the behind.
    insertStream->seekg(-position, std::ios::end);
    auto readSize = boost::iostreams::read(*insertStream, reinterpret_cast<char*>(buffer + util::HASH_SIZE), dataSize);
    if (readSize <= 0) {
      BOOST_THROW_EXCEPTION(Error("Error reading from the input stream"));
    }

    std::streambuf* buf;
    buf = std::cout.rdbuf();
    std::ostream os(buf);

    //std::cout << "Content: ";
    //os.write(reinterpret_cast<const char *>(buffer), blockSize);
    //std::cout << std::endl;

    //std::ios_base::fmtflags g(std::cout.flags());
    //std::cout << "Content(hex): " << std::hex;
    //for (int i = 0; i < (int)blockSize; i += 1) {
      //printf("%02x", buffer[i]);
    //}
    //std::cout.flags(g);
    //std::cout << std::endl;

    hash = util::calcHash(buffer, blockSize);

    //std::cout << (buffer+util::HASH_SIZE) << std::endl;

    //std::cout << "Hash: " << std::hex;
    //for (const auto& s : hash) {
      //printf("%02x", s);
    //}
    //std::cout << std::endl;
    hashes.push_front(hash);
  }

  // save first block size
  // If position >= m_bytes, only one block is generated and no hash chain
  m_firstSize = m_bytes - (position - dataSize);
  //std::cout << "first data size = " << m_firstSize << std::endl;
  insertStream->seekg(0, std::ios::beg);
}

void
NdnPutFile::prepareNextData(uint64_t referenceSegmentNo)
{
  // make sure m_data has [referenceSegmentNo, referenceSegmentNo + PRE_SIGN_DATA_COUNT] Data
  if (m_isFinished)
    return;

  size_t nDataToPrepare = PRE_SIGN_DATA_COUNT;

  if (!m_data.empty()) {
    uint64_t maxSegmentNo = m_data.rbegin()->first;

    // if what is left is less than nDataToPrepare than return. 
    if (maxSegmentNo - referenceSegmentNo >= nDataToPrepare) {
      // nothing to prepare
      return;
    }

    nDataToPrepare -= maxSegmentNo - referenceSegmentNo;
  }

  auto dataSize = blockSize - util::HASH_SIZE;
  for (size_t i = 0; i < nDataToPrepare && !m_isFinished; ++i) {
    auto segNo = referenceSegmentNo + i;

    //std::cout << "segno: " << segNo << std::endl;
    //std::cout << "hashes size: " << hashes.size() << std::endl;

    uint8_t *buffer = new uint8_t[blockSize];
    std::array<uint8_t,util::HASH_SIZE> hash;
    if (!hashes.empty()) {
      hash = hashes.front();
      hashes.pop_front();
    } else {
      hash = {0};
      m_isFinished = true;
    }


    memcpy(buffer, &hash, util::HASH_SIZE);

    auto toRead = dataSize;
    if (segNo == 0) {
      toRead = m_firstSize;
    }

    auto readSize = boost::iostreams::read(*insertStream,
                                           reinterpret_cast<char*>(buffer + util::HASH_SIZE), toRead);
    if (readSize <= 0) {
      BOOST_THROW_EXCEPTION(Error("Error reading from the input stream"));
    }

    auto data = make_shared<ndn::Data>(Name(m_dataPrefix).appendSegment(m_currentSegmentNo));
    //std::cerr<<"data name is "<<data.getName()<<std::endl;
    //std::cerr<<"data full name "<<data.getFullName() << std::endl;
    if (m_isFinished) {
      std::cout << "Finished" << std::endl;
      data->setFinalBlock(ndn::name::Component::fromSegment(m_currentSegmentNo));
    }

    data->setContent(buffer, toRead + util::HASH_SIZE);
    data->setFreshnessPeriod(freshnessPeriod);
    if(segNo == 0) {
      signFirstData(*data);
    } else {
      signData(*data);
    }

    m_data.insert(std::make_pair(m_currentSegmentNo, data));

    ++m_currentSegmentNo;
    delete[] buffer;
  }
}

void
NdnPutFile::run()
{
  m_dataPrefix = ndnName;
 start = clock();

  struct stat st;
  stat(file_name, &st);
  std::cerr<<"file name is "<<file_name<<std::endl;
  m_bytes = st.st_size;
  end = clock();
  time_result = (double)(end - start);
  printf("2nd m_bytes %d\n", m_bytes);
  printf("time for size is %f\n", time_result/CLOCKS_PER_SEC);
  start = clock();

  prepareHashes();

  //start = clock();

  if (isVerbose)
    std::cerr << "setInterestFilter for " << m_dataPrefix << std::endl;
  m_face.setInterestFilter(m_dataPrefix,
                           bind(&NdnPutFile::onInterest, this, _1, _2),
                           bind(&NdnPutFile::onRegisterSuccess, this, _1),
                           bind(&NdnPutFile::onRegisterFailed, this, _1, _2));

  if (hasTimeout)
    m_scheduler.schedule(timeout, [this] { stopProcess(); });

  m_face.processEvents();
  end = clock();
  time_result = (double)(end - start);
  printf("time is %f\n", time_result/CLOCKS_PER_SEC);
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
  std::cout<<"onInsertCommandResponse:"<<std::endl;
  RepoCommandResponse response(data.getContent().blockFromValue());
  auto statusCode = response.getCode();
  if (statusCode >= 400) {
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

  // m_keyChain.sign(*item->second, ndn::signingWithSha256());
  m_keyChain.sign(*item->second);
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
  auto dataSize = blockSize - util::HASH_SIZE;
  auto blockCount = m_bytes / dataSize + (m_bytes % dataSize != 0);

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
    //clock_t start, end;
    //start =clock();
    m_keyChain.sign(data, ndn::signingWithSha256());
    //end = clock();
    //double result = (double)(end-start);
    //printf("sign with sha256: %f\n", result/CLOCKS_PER_SEC); 
  }
  else if (identityForData.empty())
    m_keyChain.sign(data);
  else {
    //clock_t start, end;
    //start =clock();
    m_keyChain.sign(data, ndn::signingByIdentity(identityForData));
    //end = clock();
    //double result = (double)(end-start);
    //printf("sign with identity: %f\n", result/CLOCKS_PER_SEC); 
  
  }
}

void
NdnPutFile::signFirstData(ndn::Data& data)
{
if (identityForData.empty())
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

  std::cout<<"checkInterest:"<<checkInterest.toUri()<<std::endl;
  // check identity
  //m_keyChain.sign(checkInterest, ndn::signingWithSha256());
  /*
  if (identityForCommand.empty()) {
    //std::cout<<"option 1"<<std::endl;
    m_keyChain.sign(checkInterest);
  } else {
    m_keyChain.sign(checkInterest, ndn::signingByIdentity(identityForCommand));
  }
  */
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

  if (identityForCommand.empty()) {
    std::cout<<"option 1"<<std::endl;
    interest = m_cmdSigner.makeCommandInterest(cmd);
  } else {
    std::cout<<"option 2"<<std::endl;
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
            << "  -H: use Hash Chain signing method instead of Rsa\n"
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
  while ((opt = getopt(argc, argv, "hDHi:I:x:l:w:s:v")) != -1) {
    switch (opt) {
    case 'h':
      usage(argv[0]);
      return 0;
    case 'D':
      ndnPutFile.useDigestSha256 = true;
      break;
    case 'H':
      ndnPutFile.useDigestSha256 = true;
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
      if (ndnPutFile.blockSize <= util::HASH_SIZE) {
        std::cerr << "Block size cannot lte " << util::HASH_SIZE << std::endl;
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
  }

  if (argc != optind + 3) {
    usage(argv[0]);
    return 2;
  }

  argc -= optind;
  argv += optind;

  ndnPutFile.repoPrefix = Name(argv[0]);
  ndnPutFile.ndnName = Name(argv[1]);

  //std::cerr<<argv[0]<<std::endl;
  //std::cerr<<argv[1]<<std::endl;
  //std::cerr<<argv[2]<<std::endl;
  file_name = argv[2];
  if (strcmp(argv[2], "-") == 0) {
    //get file name
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

	cpu_set_t  mask;
	CPU_ZERO(&mask);
	//CPU_SET(std::thread::hardware_concurrency()-1, &mask);
	CPU_SET(4, &mask); // 0번 코어에 할당
	sched_setaffinity(4, sizeof(mask), &mask);


  try {
    return repo::main(argc, argv);
  }
  catch (const std::exception& e) {
    std::cerr << "ERROR: " << e.what() << std::endl;
    return 1;
  }
}
