#include "repo-command-parameter.hpp"
#include "repo-command-response.hpp"
#include "util.hpp"

#include "manifest/manifest.hpp"

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/command-interest-signer.hpp>
#include <ndn-cxx/security/hc-key-chain.hpp>
#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/util/scheduler.hpp>

#include "difs.hpp"
#include "consumer.hpp"

#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include <fstream>
#include <iostream>
#include <string>

#include <boost/asio.hpp>
#include <boost/iostreams/operations.hpp>
#include <boost/iostreams/read.hpp>
#include <boost/lexical_cast.hpp>

static const uint64_t DEFAULT_BLOCK_SIZE = 1000;
static const uint64_t DEFAULT_INTEREST_LIFETIME = 4000;
static const uint64_t DEFAULT_FRESHNESS_PERIOD = 10000;
static const uint64_t DEFAULT_CHECK_PERIOD = 1000;
static const size_t PRE_SIGN_DATA_COUNT = 11;

namespace difs {

using ndn::Name;
using ndn::Interest;
using ndn::Data;
using ndn::Block;

using std::bind;
using std::placeholders::_1;
using std::placeholders::_2;

using namespace repo;

static const int MAX_RETRY = 3;

void
DIFS::run()
{
  m_face.processEvents();
}

// Set

void 
DIFS::setNodePrefix(ndn::DelegationList nodePrefix) 
{
  m_nodePrefix = nodePrefix;
}

void 
DIFS::setForwardingHint(ndn::DelegationList forwardingHint) 
{
  m_forwardingHint = forwardingHint;
}

void
DIFS::setRepoPrefix(ndn::Name repoPrefix) 
{
  m_repoPrefix = repoPrefix;
}

void
DIFS::setTimeOut(ndn::time::milliseconds timeout) 
{
  m_timeout = timeout;
}

void
DIFS::setInterestLifetime(ndn::time::milliseconds interestLifetime) 
{
  m_interestLifetime = interestLifetime;
}

void
DIFS::setFreshnessPeriod(ndn::time::milliseconds freshnessPeriod) 
{
  m_freshnessPeriod = freshnessPeriod;
}

void
DIFS::setHasTimeout(bool hasTimeout) 
{
  m_hasTimeout = hasTimeout;
}

void
DIFS::setVerbose(bool verbose) 
{
  m_verbose = verbose;
}

void
DIFS::setUseDigestSha256(bool useDigestSha256) 
{
  m_useDigestSha256 = useDigestSha256;
}

void
DIFS::setBlockSize(size_t blockSize) 
{
  m_blockSize = blockSize;
}

void
DIFS::setIdentityForData(std::string identityForData) 
{
  m_identityForData = identityForData;
}

void
DIFS::setIdentityForCommand(std::string identityForCommand) 
{
  m_identityForCommand = identityForCommand;
}

// Delete Node
void
DIFS::deleteNode(const std::string from, const std::string to)
{
  RepoCommandParameter parameter;
  parameter.setFrom(ndn::encoding::makeBinaryBlock(tlv::From, from.c_str(), from.length()));
  parameter.setTo(ndn::encoding::makeBinaryBlock(tlv::To, to.c_str(), to.length()));

  Name cmd = m_common_name;
  cmd.append("del-node")
    .append(parameter.wireEncode());

  ndn::Interest deleteNodeInterest = m_cmdSigner.makeCommandInterest(cmd);
  if(!m_forwardingHint.empty())
    deleteNodeInterest.setForwardingHint(m_forwardingHint);
  deleteNodeInterest.setInterestLifetime(m_interestLifetime);
  deleteNodeInterest.setMustBeFresh(true);

  m_face.expressInterest(deleteNodeInterest,
                        std::bind(&DIFS::onDeleteNodeCommandResponse, this, _1, _2),
                        std::bind(&DIFS::onDeleteNodeCommandNack, this, _1), // Nack
                        std::bind(&DIFS::onDeleteNodeCommandTimeout, this, _1));
}

void
DIFS::onDeleteNodeCommandResponse(const ndn::Interest& interest, const ndn::Data& data)
{
  // RepoCommandResponse response(data.getContent().blockFromValue());
  // int statusCode = response.getCode();
  // if (statusCode == 404) {
  //   std::cerr << "Manifest not found" << std::endl;
  //   return;
  // }
  // else if (statusCode >= 400) {
  //   std::cerr << "delete command failed with code " << statusCode << interest.getName() << std::endl;
  //   return;
  // }
}

void
DIFS::onDeleteNodeCommandTimeout(const Interest& interest)
{
  // if (m_retryCount++ < MAX_RETRY) {
  //   deleteFile(interest.getName());
  //   if (m_verbose) {
  //     std::cerr << "TIMEOUT: retransmit interest for " << interest.getName() << std::endl;
  //   }
  // } else {
  //   std::cerr << "TIMEOUT: last interest sent" << std::endl
  //   << "TIMEOUT: abort fetching after " << MAX_RETRY << " times of retry" << std::endl;
  // }
}

void
DIFS::onDeleteNodeCommandNack(const Interest& interest)
{
  // if (m_retryCount++ < MAX_RETRY) {
  //   deleteFile(interest.getName());
  //   if (m_verbose) {
  //     std::cerr << "NACK: retransmit interest for " << interest.getName() << std::endl;
  //   }
  // } else {
  //   std::cerr << "NACK: last interest sent" << std::endl
  //   << "NACK: abort fetching after " << MAX_RETRY << " times of retry" << std::endl;
  // }
}

// Delete
void
DIFS::deleteFile(const Name& name)
{
  RepoCommandParameter parameter;
  parameter.setProcessId(0);  // FIXME: set process id properly
  parameter.setName(name);

  Name cmd = m_common_name;
  cmd.append("delete")
    .append(parameter.wireEncode());

  ndn::Interest commandInterest = m_cmdSigner.makeCommandInterest(cmd);
  if(!m_forwardingHint.empty())
    commandInterest.setForwardingHint(m_forwardingHint);
  commandInterest.setInterestLifetime(m_interestLifetime);
  commandInterest.setMustBeFresh(true);

  m_face.expressInterest(commandInterest,
                        std::bind(&DIFS::onDeleteCommandResponse, this, _1, _2),
                        std::bind(&DIFS::onDeleteCommandNack, this, _1), // Nack
                        std::bind(&DIFS::onDeleteCommandTimeout, this, _1));
}

void
DIFS::onDeleteCommandResponse(const ndn::Interest& interest, const ndn::Data& data)
{
  RepoCommandResponse response(data.getContent().blockFromValue());
  int statusCode = response.getCode();
  if (statusCode == 404) {
    std::cerr << "Manifest not found" << std::endl;
    return;
  }
  else if (statusCode >= 400) {
    std::cerr << "delete command failed with code " << statusCode << interest.getName() << std::endl;
    return;
  }
}

void
DIFS::onDeleteCommandTimeout(const Interest& interest)
{
  if (m_retryCount++ < MAX_RETRY) {
    deleteFile(interest.getName());
    if (m_verbose) {
      std::cerr << "TIMEOUT: retransmit interest for " << interest.getName() << std::endl;
    }
  } else {
    std::cerr << "TIMEOUT: last interest sent" << std::endl
    << "TIMEOUT: abort fetching after " << MAX_RETRY << " times of retry" << std::endl;
  }
}

void
DIFS::onDeleteCommandNack(const Interest& interest)
{
  if (m_retryCount++ < MAX_RETRY) {
    deleteFile(interest.getName());
    if (m_verbose) {
      std::cerr << "NACK: retransmit interest for " << interest.getName() << std::endl;
    }
  } else {
    std::cerr << "NACK: last interest sent" << std::endl
    << "NACK: abort fetching after " << MAX_RETRY << " times of retry" << std::endl;
  }
}

void
DIFS::getInfo() 
{
  RepoCommandParameter parameter;

  Name cmd = m_repoPrefix;
  cmd.append("info")
    .append(parameter.wireEncode());

  ndn::Interest commandInterest = m_cmdSigner.makeCommandInterest(cmd);
  commandInterest.setInterestLifetime(m_interestLifetime);
  if(!m_forwardingHint.empty()) {
    commandInterest.setForwardingHint(m_forwardingHint);
  }

  m_face.expressInterest(commandInterest,
                        std::bind(&DIFS::onGetInfoCommandResponse, this, _1, _2),
                        std::bind(&DIFS::onGetInfoCommandNack, this, _1), // Nack
                        std::bind(&DIFS::onGetInfoCommandTimeout, this, _1));
}

void
DIFS::onGetInfoCommandResponse(const ndn::Interest& interest, const ndn::Data& data)
{ 
  std::cout << data.getContent().value() << std::endl;
}

void
DIFS::onGetInfoCommandTimeout(const Interest& interest)
{
  if (m_retryCount++ < MAX_RETRY) {
    getInfo();
    if (m_verbose) {
      std::cerr << "TIMEOUT: retransmit interest for " << interest.getName() << std::endl;
    }
  } else {
    std::cerr << "TIMEOUT: last interest sent" << std::endl
    << "TIMEOUT: abort fetching after " << MAX_RETRY << " times of retry" << std::endl;
  }
}

void
DIFS::onGetInfoCommandNack(const Interest& interest)
{
  if (m_retryCount++ < MAX_RETRY) {
    getInfo();
    if (m_verbose) {
      std::cerr << "NACK: retransmit interest for " << interest.getName() << std::endl;
    }
  } else {
    std::cerr << "NACK: last interest sent" << std::endl
    << "NACK: abort fetching after " << MAX_RETRY << " times of retry" << std::endl;
  }
}

void
DIFS::getKeySpaceInfo() 
{
  RepoCommandParameter parameter;

  Name cmd = m_repoPrefix;
  cmd.append("ringInfo")
    .append(parameter.wireEncode());

  ndn::Interest commandInterest = m_cmdSigner.makeCommandInterest(cmd);
  commandInterest.setInterestLifetime(m_interestLifetime);
  if(!m_forwardingHint.empty()) {
    commandInterest.setForwardingHint(m_forwardingHint);
  }

  m_face.expressInterest(commandInterest,
                        std::bind(&DIFS::onGetInfoCommandResponse, this, _1, _2),
                        std::bind(&DIFS::onGetInfoCommandNack, this, _1), // Nack
                        std::bind(&DIFS::onGetInfoCommandTimeout, this, _1));
}

void
DIFS::onGetKeySpaceInfoCommandResponse(const ndn::Interest& interest, const ndn::Data& data)
{ 
  std::cout << data.getContent().value() << std::endl;
}

void
DIFS::onGetKeySpaceInfoCommandTimeout(const Interest& interest)
{
  if (m_retryCount++ < MAX_RETRY) {
    getKeySpaceInfo();
    if (m_verbose) {
      std::cerr << "TIMEOUT: retransmit interest for " << interest.getName() << std::endl;
    }
  } else {
    std::cerr << "TIMEOUT: last interest sent" << std::endl
    << "TIMEOUT: abort fetching after " << MAX_RETRY << " times of retry" << std::endl;
  }
}

void
DIFS::onGetKeySpaceInfoCommandNack(const Interest& interest)
{
  if (m_retryCount++ < MAX_RETRY) {
    getKeySpaceInfo();
    if (m_verbose) {
      std::cerr << "NACK: retransmit interest for " << interest.getName() << std::endl;
    }
  } else {
    std::cerr << "NACK: last interest sent" << std::endl
    << "NACK: abort fetching after " << MAX_RETRY << " times of retry" << std::endl;
  }
}

// Get

void
DIFS::getFile(const Name& data_name, std::ostream& os)
{
  RepoCommandParameter parameter;
  parameter.setName(data_name);

  m_os = &os;
  Name cmd = m_repoPrefix;
  cmd.append("get")
    .append(parameter.wireEncode());

  ndn::Interest commandInterest = m_cmdSigner.makeCommandInterest(cmd);
  commandInterest.setInterestLifetime(m_interestLifetime);
  if(!m_forwardingHint.empty()) {
    commandInterest.setForwardingHint(m_forwardingHint);
  }
  commandInterest.setCanBePrefix(true);
  commandInterest.setMustBeFresh(true);

  m_face.expressInterest(commandInterest,
                        std::bind(&DIFS::onGetCommandResponse, this, _1, _2),
                        std::bind(&DIFS::onGetCommandNack, this, _1),
                        std::bind(&DIFS::onGetCommandTimeout, this, _1));
}

void
DIFS::onGetCommandResponse(const Interest& interest, const Data& data)
{
  auto content = data.getContent();
  std::string json(
    content.value_begin(),
    content.value_end()
  );

  if (json.length() == 0) {
    std::cerr << "Not found" << std::endl;
    return;
  }

  m_manifest = json;

  // m_manifest = Manifest::fromJson(json);

  fetch(0);
  // Consumer consumer(m_face, manifest, *m_os);
  // consumer.fetch(0);
  // consumer.run();
}

void
DIFS::onGetCommandTimeout(const Interest& interest)
{
  if (m_retryCount++ < MAX_RETRY) {
    getFile(interest.getName(), *m_os);
    if (m_verbose) {
      std::cerr << "TIMEOUT: retransmit interest for " << interest.getName() << std::endl;
    }
  } else {
    std::cerr << "TIMEOUT: last interest sent" << std::endl
    << "TIMEOUT: abort fetching after " << MAX_RETRY << " times of retry" << std::endl;
  }
}

void
DIFS::onGetCommandNack(const Interest& interest)
{
  if (m_retryCount++ < MAX_RETRY) {
    getFile(interest.getName(), *m_os);
    if (m_verbose) {
      std::cerr << "TIMEOUT: retransmit interest for " << interest.getName() << std::endl;
    }
  } else {
    std::cerr << "TIMEOUT: last interest sent" << std::endl
    << "TIMEOUT: abort fetching after " << MAX_RETRY << " times of retry" << std::endl;
  }
}

void
DIFS::fetch(int start)
{
  auto manifest = Manifest::fromJson(m_manifest);
  auto repos = manifest.getRepos();

  for (auto iter = repos.begin(); iter != repos.end(); ++iter)
  {
    for (int segment_id = start; segment_id < start + 100; segment_id++)
    {
      if(segment_id > iter->end)
      	break;

      ndn::Interest interest(ndn::Name(iter->name).append("data").append(manifest.getName()).appendSegment(segment_id));
      boost::chrono::milliseconds lifeTime(4000);
      interest.setInterestLifetime(lifeTime);
      interest.setMustBeFresh(true);

      // ndn::util::SegmentFetcher::Options options;
      // options.initCwnd = initialCredit;
      // options.interestLifetime = m_interestLifetime;
      // options.maxTimeout = m_maxTimeout;

      // std::shared_ptr<ndn::util::HCSegmentFetcher> hc_fetcher;
      // auto hcFetcher = hc_fetcher->start(m_face, interest, m_validator, options);
      // hcFetcher->onError.connect([](uint32_t errorCode, const std::string &errorMsg)
			// 	 { NDN_LOG_ERROR("Error: " << errorMsg); });
      // hcFetcher->afterSegmentValidated.connect([this, hcFetcher, processId](const Data &data)
			// 		       { onDataCommandResponse(); });
      // hcFetcher->afterSegmentTimedOut.connect([this, hcFetcher, processId]()
			// 		      { on(*hcFetcher, processId); });

      m_face.expressInterest(interest,
          std::bind(&DIFS::onDataCommandResponse, this, _1, _2),
          std::bind(&DIFS::onDataCommandNack, this, _1), // Nack
          std::bind(&DIFS::onDataCommandTimeout, this, _1));
    }
  }
}

void 
DIFS::onFetchInterest(const ndn::Interest& interest)
{
  m_face.expressInterest(interest,
      std::bind(&DIFS::onDataCommandResponse, this, _1, _2),
      std::bind(&DIFS::onDataCommandNack, this, _1), // Nack
      std::bind(&DIFS::onDataCommandTimeout, this, _1));
}

void
DIFS::onDataCommandResponse(const ndn::Interest& interest, const ndn::Data& data)
{
  const auto &content = data.getContent();
  content.parse();

  ndn::Name::Component segmentComponent = interest.getName().get(-1);
  uint64_t segmentNo = segmentComponent.toSegment();

  ndn::Name::Component endBlockComponent = data.getFinalBlock().value();
  uint64_t endNo = endBlockComponent.toSegment();

  map.insert(std::pair<int, const ndn::Block>(segmentNo, content.get(ndn::tlv::Content)));

  if(map.size() - 1 == endNo) {
    for(auto iter = map.begin(); iter != map.end(); iter++) {
      m_os->write(reinterpret_cast<const char *>(iter->second.value()), iter->second.value_size());
      m_totalSize += iter->second.value_size();
      m_currentSegment += 1;
    }

    std::cerr << "INFO: End of file is reached" << std::endl;
    std::cerr << "INFO: Total # of segments received: " << m_currentSegment << std::endl;
    std::cerr << "INFO: Total # bytes of content received: " << m_totalSize << std::endl;
  }

  if(segmentNo % 100 == 99) {
    fetch((int)segmentNo + 1);
  }
}

void
DIFS::onDataCommandTimeout(const ndn::Interest& interest)
{
  if(m_retryCount++ < 3) {
    onFetchInterest(interest);
    if (m_verbose) {
      std::cerr << "TIMEOUT: retransmit interest for " << interest.getName() << std::endl;
    }
  } else {
    std::cerr << "TIMEOUT: last interest sent" << std::endl
      << "TIMEOUT: abort fetching after " << 3 << " times of retry" << std::endl;
  }
}

void
DIFS::onDataCommandNack(const ndn::Interest& interest)
{
  if(m_retryCount++ < 3) {
    onFetchInterest(interest);
    if (m_verbose) {
      std::cerr << "TIMEOUT: retransmit interest for " << interest.getName() << std::endl;
    }
  } else {
    std::cerr << "NACK: last interest sent" << std::endl
      << "NACK: abort fetching after " << 3 << " times of retry" << std::endl;
  }
}

// Put

void
DIFS::putFile(const ndn::Name& ndnName, std::istream& is)
{
  m_dataPrefix = ndnName;
  m_ndnName = ndnName;

  m_insertStream = &is;
  m_insertStream->seekg(0, std::ios::beg);
  auto beginPos = m_insertStream->tellg();
  m_insertStream->seekg(0, std::ios::end);
  m_bytes = m_insertStream->tellg() - beginPos;
  m_insertStream->seekg(0, std::ios::beg);

  putFilePrepareNextData();

  m_face.setInterestFilter(m_dataPrefix,
                           bind(&DIFS::onPutFileInterest, this, _1, _2),
                           bind(&DIFS::onPutFileRegisterSuccess, this, _1),
                           bind(&DIFS::onPutFileRegisterFailed, this, _1, _2));

  if (m_hasTimeout)
    m_scheduler.schedule(timeout, [this] { putFileStopProcess(); });
}


void
DIFS::onPutFileInterest(const ndn::Name& prefix, const ndn::Interest& interest)
{
  if (interest.getName().size() == prefix.size()) {
    putFileSendManifest(prefix, interest);
    return;
  }

  uint64_t segmentNo;
  try {
    ndn::Name::Component segmentComponent = interest.getName().get(prefix.size());
    segmentNo = segmentComponent.toSegment();
  }
  catch (const tlv::Error& e) {
    if (m_verbose) {
      std::cerr << "Error processing incoming interest " << interest << ": "
                << e.what() << std::endl;
    }
    return;
  }

  shared_ptr<Data> data;
  if (segmentNo < m_data.size()) {
    data = m_data[segmentNo];
  } else if (interest.matchesData(*m_data[0])) {
    data = m_data[0];
  }

  if (data != nullptr) {
    m_face.put(*data);
  }
  else {
    m_face.put(ndn::lp::Nack(interest));
  }
}

void
DIFS::onPutFileRegisterSuccess(const Name& prefix)
{
  putFileStartInsertCommand();
}

void
DIFS::onPutFileRegisterFailed(const ndn::Name& prefix, const std::string& reason) {
  BOOST_THROW_EXCEPTION(std::runtime_error("onRegisterFailed: " + reason));
}

void
DIFS::putFileStartInsertCommand()
{
  RepoCommandParameter parameters;
  parameters.setName(m_dataPrefix);

  if(!m_nodePrefix.empty())
    parameters.setNodePrefix(m_nodePrefix);

  Name cmd = m_repoPrefix;
  cmd
    .append("insert")
    .append(parameters.wireEncode());

  ndn::Interest commandInterest;
  if (m_identityForCommand.empty())
    commandInterest = m_cmdSigner.makeCommandInterest(cmd);
  else {
    commandInterest = m_cmdSigner.makeCommandInterest(cmd, ndn::signingByIdentity(m_identityForCommand));
  }

  if(!m_forwardingHint.empty())
    commandInterest.setForwardingHint(m_forwardingHint);
  commandInterest.setInterestLifetime(m_interestLifetime);

  m_face.expressInterest(commandInterest,
                         bind(&DIFS::onPutFileInsertCommandResponse, this, _1, _2),
                         bind(&DIFS::onPutFileInsertCommandTimeout, this, _1), // Nack
                         bind(&DIFS::onPutFileInsertCommandNack, this, _1));
}

void
DIFS::onPutFileInsertCommandResponse(const ndn::Interest& interest, const ndn::Data& data)
{
  RepoCommandResponse response(data.getContent().blockFromValue());
  auto statusCode = response.getCode();
  if (statusCode >= 400) {
    BOOST_THROW_EXCEPTION(std::runtime_error("insert command failed with code: " + 
                                              boost::lexical_cast<std::string>(statusCode)));
  }
  m_processId = response.getProcessId();

  m_scheduler.schedule(m_checkPeriod, [this] { putFileStartCheckCommand(); });
}

void
DIFS::onPutFileInsertCommandTimeout(const ndn::Interest& interest)
{
	if(m_retryCount++ < MAX_RETRY) {
		putFileStartInsertCommand();
    if (m_verbose) {
      std::cerr << "TIMEOUT: retransmit interest for " << interest.getName() << std::endl;
    }
	} else {
		std::cerr << "TIMEOUT: last interest sent" << std::endl
		<< "TIMEOUT: abort fetching after " << MAX_RETRY << " times of retry" << std::endl;
	}
}

void
DIFS::onPutFileInsertCommandNack(const ndn::Interest& interest)
{
	if(m_retryCount++ < MAX_RETRY) {
		putFileStartInsertCommand();
    if (m_verbose) {
      std::cerr << "TIMEOUT: retransmit interest for " << interest.getName() << std::endl;
    }
	} else {
		std::cerr << "NACK: last interest sent" << std::endl
		<< "NACK: abort fetching after " << MAX_RETRY << " times of retry" << std::endl;
	}
}

void
DIFS::putFileStartCheckCommand()
{
  auto parameter = RepoCommandParameter();
  parameter.setName(m_ndnName);

  if(!m_nodePrefix.empty())
    parameter.setNodePrefix(m_nodePrefix);

  Name cmd = m_repoPrefix;
  cmd
    .append("insert check")
    .append(parameter.setProcessId(m_processId).wireEncode());

  ndn::Interest checkInterest;
  if (m_identityForCommand.empty())
    checkInterest = m_cmdSigner.makeCommandInterest(cmd);
  else {
    checkInterest = m_cmdSigner.makeCommandInterest(cmd, ndn::signingByIdentity(m_identityForCommand));
  }

  if(!m_forwardingHint.empty())
    checkInterest.setForwardingHint(m_forwardingHint);

  checkInterest.setInterestLifetime(m_interestLifetime);

  m_face.expressInterest(checkInterest,
                         bind(&DIFS::onPutFileCheckCommandResponse, this, _1, _2),
                         bind(&DIFS::onPutFileCheckCommandTimeout, this, _1), // Nack
                         bind(&DIFS::onPutFileCheckCommandTimeout, this, _1));
}

void
DIFS::onPutFileCheckCommandResponse(const ndn::Interest& interest, const ndn::Data& data)
{
  RepoCommandResponse response(data.getContent().blockFromValue());
  auto statusCode = response.getCode();
  if (statusCode >= 400) {
    BOOST_THROW_EXCEPTION(std::runtime_error("Insert check command failed with code: " + 
                                              boost::lexical_cast<std::string>(statusCode)));
  }

  uint64_t insertCount = response.getInsertNum();

  // Technically, the check should not infer, but directly has signal from repo that
  // write operation has been finished

  if (insertCount == m_currentSegmentNo) {
    m_face.getIoService().stop();
    return;
  }

  m_scheduler.schedule(m_checkPeriod, [this] { putFileStartCheckCommand(); });
}

void
DIFS::onPutFileCheckCommandTimeout(const ndn::Interest& interest)
{
	if(m_retryCount++ < MAX_RETRY) {
		putFileStartCheckCommand();
    if (m_verbose) {
      std::cerr << "TIMEOUT: retransmit interest for " << interest.getName() << std::endl;
    }
	} else {
		std::cerr << "TIMEOUT: last interest sent" << std::endl
		<< "TIMEOUT: abort fetching after " << MAX_RETRY << " times of retry" << std::endl;
	}
}

void
DIFS::onPutFileCheckCommandNack(const ndn::Interest& interest)
{
	if(m_retryCount++ < MAX_RETRY) {
		putFileStartCheckCommand();
    if (m_verbose) {
      std::cerr << "TIMEOUT: retransmit interest for " << interest.getName() << std::endl;
    }
	} else {
		std::cerr << "NACK: last interest sent" << std::endl
		<< "NACK: abort fetching after " << MAX_RETRY << " times of retry" << std::endl;
	}
}

void
DIFS::putFilePrepareNextData()
{
  int chunkSize = m_bytes / m_blockSize;
  int lastDataSize = m_bytes % m_blockSize;
  auto finalBlockId = ndn::name::Component::fromSegment(chunkSize);

  std::vector<uint8_t> buffer(m_blockSize);
  Block nextHash(tlv::SignatureValue);

  for(int count = 0; count <= chunkSize; count++) {
    if(count == 0) {
      m_insertStream->seekg(m_bytes - lastDataSize);
      m_insertStream->read(reinterpret_cast<char *>(buffer.data()), lastDataSize);     
    } else {
      m_insertStream->seekg(m_bytes - lastDataSize - buffer.size() * count);
      m_insertStream->read(reinterpret_cast<char *>(buffer.data()), buffer.size());
    }

    const auto nCharsRead = m_insertStream->gcount();

    if(nCharsRead > 0) {
      auto data = std::make_shared<ndn::Data>(Name(m_dataPrefix).appendSegment(chunkSize - count));
      data->setFreshnessPeriod(m_freshnessPeriod);
      Block content = ndn::encoding::makeBinaryBlock(tlv::Content, buffer.data(), nCharsRead);
      data->setContent(content);
      data->setFinalBlock(finalBlockId);

      if(count == chunkSize) {
        m_hcKeyChain.sign(*data, nextHash);
      } else {
        m_hcKeyChain.sign(*data, nextHash, ndn::signingWithSha256());
      }

      nextHash = data->getSignatureValue();

      m_data.insert(m_data.begin(), data);
      m_currentSegmentNo++;
    } else {
      std::cerr << "Data read failed" << std::endl;
      return;
    }
  }
}

void
DIFS::putFileSendManifest(const ndn::Name& prefix, const ndn::Interest& interest)
{
  BOOST_ASSERT(prefix == m_dataPrefix);

  if (prefix != interest.getName()) {
    // if (m_verbose) {
      std::cerr << "Received unexpected interest " << interest << std::endl;
    // }
    return;
  }

  ndn::Data data(interest.getName());
  auto blockCount = m_bytes / m_blockSize + (m_bytes % m_blockSize != 0);

  Manifest manifest(interest.getName().toUri(), 0, blockCount - 1);
  std::string json = manifest.toInfoJson();
  data.setContent((uint8_t*) json.data(), (size_t) json.size());
  data.setFreshnessPeriod(3_s);
  m_hcKeyChain.ndn::KeyChain::sign(data);

  m_face.put(data);
}

void
DIFS::putFileStopProcess()
{
  m_face.getIoService().stop();
}
}
