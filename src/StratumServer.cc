/*
 The MIT License (MIT)

 Copyright (c) [2016] [BTC.COM]

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
 */
#include "StratumServer.h"
#include "StratumSession.h"
#include "DiffController.h"

#include <algorithm>
#include <boost/thread.hpp>

using namespace std;

#ifndef WORK_WITH_STRATUM_SWITCHER

//////////////////////////////// SessionIDManagerT //////////////////////////////
template <uint8_t IBITS>
SessionIDManagerT<IBITS>::SessionIDManagerT(const uint8_t serverId) :
serverId_(serverId), count_(0), allocIdx_(0), allocInterval_(0)
{
  static_assert(IBITS <= 24, "IBITS cannot large than 24");
  sessionIds_.reset();
}

template <uint8_t IBITS>
bool SessionIDManagerT<IBITS>::ifFull() {
  ScopeLock sl(lock_);
  return _ifFull();
}

template <uint8_t IBITS>
bool SessionIDManagerT<IBITS>::_ifFull() {
  if (count_ > kSessionIdMask) {
    return true;
  }
  return false;
}

template <uint8_t IBITS>
void SessionIDManagerT<IBITS>::setAllocInterval(uint32_t interval) {
  allocInterval_ = interval;
}

template <uint8_t IBITS>
bool SessionIDManagerT<IBITS>::allocSessionId(uint32_t *sessionID) {
  ScopeLock sl(lock_);

  if (_ifFull())
    return false;

  // find an empty bit
  while (sessionIds_.test(allocIdx_) == true) {
    allocIdx_ = (allocIdx_ + 1) & kSessionIdMask;
  }

  // set to true
  sessionIds_.set(allocIdx_, true);
  count_++;

  *sessionID = (((uint32_t)serverId_ << IBITS) | allocIdx_);
  allocIdx_ = (allocIdx_ + allocInterval_) & kSessionIdMask;
  return true;
}

template <uint8_t IBITS>
void SessionIDManagerT<IBITS>::freeSessionId(uint32_t sessionId) {
  ScopeLock sl(lock_);

  const uint32_t idx = (sessionId & kSessionIdMask);
  sessionIds_.set(idx, false);
  count_--;
}

// Class template instantiation
template class SessionIDManagerT<8>;
template class SessionIDManagerT<16>;
template class SessionIDManagerT<24>;

#endif // #ifndef WORK_WITH_STRATUM_SWITCHER


////////////////////////////////// JobRepository ///////////////////////////////
JobRepository::JobRepository(const char *kafkaBrokers, const char *consumerTopic, const string &fileLastNotifyTime, StratumServer *server)
  : running_(true)
  , kafkaConsumer_(kafkaBrokers, consumerTopic, 0/*patition*/)
  , server_(server), fileLastNotifyTime_(fileLastNotifyTime)
  , kMaxJobsLifeTime_(300)
  , kMiningNotifyInterval_(30)  // TODO: make as config arg
  , lastJobSendTime_(0)
{
  assert(kMiningNotifyInterval_ < kMaxJobsLifeTime_);
}

JobRepository::~JobRepository() {
  if (threadConsume_.joinable())
    threadConsume_.join();
}

void JobRepository::setMaxJobDelay (const time_t maxJobDelay) {
  LOG(INFO) << "set max job delay to " << maxJobDelay << "s";
  kMaxJobsLifeTime_ = maxJobDelay;
}

shared_ptr<StratumJobEx> JobRepository::getStratumJobEx(const uint64_t jobId) {
  ScopeLock sl(lock_);
  auto itr = exJobs_.find(jobId);
  if (itr != exJobs_.end()) {
    return itr->second;
  }
  return nullptr;
}

shared_ptr<StratumJobEx> JobRepository::getLatestStratumJobEx() {
  ScopeLock sl(lock_);
  if (exJobs_.size()) {
    return exJobs_.rbegin()->second;
  }
  LOG(WARNING) << "getLatestStratumJobEx fail";
  return nullptr;
}

void JobRepository::stop() {
  if (!running_) {
    return;
  }
  running_ = false;
  LOG(INFO) << "stop job repository";
}

bool JobRepository::setupThreadConsume() {
  const int32_t kConsumeLatestN = 1;

  // we need to consume the latest one
  map<string, string> consumerOptions;
  consumerOptions["fetch.wait.max.ms"] = "10";
  if (kafkaConsumer_.setup(RD_KAFKA_OFFSET_TAIL(kConsumeLatestN),
                           &consumerOptions) == false) {
    LOG(INFO) << "setup consumer fail";
    return false;
  }

  if (!kafkaConsumer_.checkAlive()) {
    LOG(ERROR) << "kafka brokers is not alive";
    return false;
  }

  threadConsume_ = thread(&JobRepository::runThreadConsume, this);
  return true;
}

void JobRepository::runThreadConsume() {
  LOG(INFO) << "start job repository consume thread";

  const int32_t kTimeoutMs = 1000;
  while (running_) {
    rd_kafka_message_t *rkmessage;
    rkmessage = kafkaConsumer_.consumer(kTimeoutMs);

    // timeout, most of time it's not nullptr and set an error:
    //          rkmessage->err == RD_KAFKA_RESP_ERR__PARTITION_EOF
    if (rkmessage == nullptr) {
      continue;
    }

    // consume stratum job
    //
    // It will create a StratumJob and try to broadcast it immediately with broadcastStratumJob(StratumJob *).
    // A derived class needs to implement the abstract method broadcastStratumJob(StratumJob *) to decide
    // whether to add the StratumJob to the map exJobs_ and whether to send the job to miners immediately.
    // Derived classes do not need to implement a scheduled sending mechanism, checkAndSendMiningNotify() will
    // provide a default implementation.
    consumeStratumJob(rkmessage);
    
    // Return message to rdkafka
    rd_kafka_message_destroy(rkmessage);  

    // check if we need to send mining notify
    // It's a default implementation of scheduled sending / regular updating of stratum jobs.
    // If no job is sent for a long time via broadcastStratumJob(), a job will be sent via this method.
    checkAndSendMiningNotify();

    tryCleanExpiredJobs();
  }
  LOG(INFO) << "stop job repository consume thread";
}



void JobRepository::consumeStratumJob(rd_kafka_message_t *rkmessage) {
  // check error
  if (rkmessage->err) {
    if (rkmessage->err == RD_KAFKA_RESP_ERR__PARTITION_EOF) {
      // Reached the end of the topic+partition queue on the broker.
      // Not really an error.
      //      LOG(INFO) << "consumer reached end of " << rd_kafka_topic_name(rkmessage->rkt)
      //      << "[" << rkmessage->partition << "] "
      //      << " message queue at offset " << rkmessage->offset;
      // acturlly
      return;
    }

    LOG(ERROR) << "consume error for topic " << rd_kafka_topic_name(rkmessage->rkt)
    << "[" << rkmessage->partition << "] offset " << rkmessage->offset
    << ": " << rd_kafka_message_errstr(rkmessage);

    if (rkmessage->err == RD_KAFKA_RESP_ERR__UNKNOWN_PARTITION ||
        rkmessage->err == RD_KAFKA_RESP_ERR__UNKNOWN_TOPIC) {
      LOG(FATAL) << "consume fatal";
    }
    return;
  }

  StratumJob *sjob = createStratumJob();
  bool res = sjob->unserializeFromJson((const char *)rkmessage->payload,
                                       rkmessage->len);
  if (res == false) {
    LOG(ERROR) << "unserialize stratum job fail";
    delete sjob;
    return;
  }
  // make sure the job is not expired.
  time_t now = time(nullptr);
  if (sjob->jobTime() + kMaxJobsLifeTime_ < now) {
    LOG(ERROR) << "too large delay from kafka to receive topic 'StratumJob' job time=" << sjob->jobTime() << ", max delay=" << kMaxJobsLifeTime_ << ", now=" << now;
    delete sjob;
    return;
  }
  // here you could use Map.find() without lock, it's sure
  // that everyone is using this Map readonly now
  auto existingJob = getStratumJobEx(sjob->jobId_);
  if(existingJob != nullptr)
  {
    LOG(ERROR) << "jobId already existed";
    delete sjob;
    return;
  }

  broadcastStratumJob(sjob);
}

StratumJobEx* JobRepository::createStratumJobEx(StratumJob *sjob, bool isClean){
  return new StratumJobEx(sjob, isClean);
}

void JobRepository::markAllJobsAsStale() {
  ScopeLock sl(lock_);
  for (auto it : exJobs_) {
    it.second->markStale();
  }
}

void JobRepository::checkAndSendMiningNotify() {
  // last job is 'expried', send a new one
  if (exJobs_.size() &&
      lastJobSendTime_ + kMiningNotifyInterval_ <= time(nullptr))
  {
    shared_ptr<StratumJobEx> exJob = exJobs_.rbegin()->second;
    sendMiningNotify(exJob);
  }
}

void JobRepository::sendMiningNotify(shared_ptr<StratumJobEx> exJob) {
  static uint64_t lastJobId = 0;
  if (lastJobId == exJob->sjob_->jobId_) {
    LOG(ERROR) << "no new jobId, ignore to send mining notify";
    return;
  }

  // send job to all clients
  server_->sendMiningNotifyToAll(exJob);
  lastJobSendTime_ = time(nullptr);
  lastJobId = exJob->sjob_->jobId_;

  // write last mining notify time to file
  if (!fileLastNotifyTime_.empty())
    writeTime2File(fileLastNotifyTime_.c_str(), (uint32_t)lastJobSendTime_);
}

void JobRepository::tryCleanExpiredJobs() {
  ScopeLock sl(lock_);

  const uint32_t nowTs = (uint32_t)time(nullptr);
  while (exJobs_.size()) {
    // Maps (and sets) are sorted, so the first element is the smallest,
    // and the last element is the largest.
    auto itr = exJobs_.begin();

    const time_t jobTime = (time_t)(itr->first >> 32);
    if (nowTs < jobTime + kMaxJobsLifeTime_) {
      break;  // not expired
    }

    // remove expired job
    exJobs_.erase(itr);

    LOG(INFO) << "remove expired stratum job, id: " << itr->first
    << ", time: " << date("%F %T", jobTime);
  }
}





//////////////////////////////////// UserInfo /////////////////////////////////
UserInfo::UserInfo(StratumServer *server, const libconfig::Config &config):
running_(true), lastMaxUserId_(0),
caseInsensitive_(true),
server_(server)
{
  // optional
  config.lookupValue("users.case_insensitive", caseInsensitive_);
  // required (exception will be threw if inexists)
  apiUrl_ = config.lookup("users.list_id_api_url").c_str();
  
  pthread_rwlock_init(&rwlock_, nullptr);
}

UserInfo::~UserInfo() {
  stop();

  if (threadUpdate_.joinable())
    threadUpdate_.join();

  if (threadInsertWorkerName_.joinable())
    threadInsertWorkerName_.join();

  pthread_rwlock_destroy(&rwlock_);
}

void UserInfo::stop() {
  if (!running_)
    return;

  running_ = false;
}

void UserInfo::regularUserName(string &userName) {
  if (caseInsensitive_) {
    std::transform(userName.begin(), userName.end(), userName.begin(), ::tolower);
  }
}

int32_t UserInfo::getUserId(string userName) {
  regularUserName(userName);

  pthread_rwlock_rdlock(&rwlock_);
  auto itr = nameIds_.find(userName);
  pthread_rwlock_unlock(&rwlock_);

  if (itr != nameIds_.end()) {
    return itr->second;
  }
  return 0;  // not found
}

#ifdef USER_DEFINED_COINBASE
////////////////////// User defined coinbase enabled //////////////////////

// getCoinbaseInfo
string UserInfo::getCoinbaseInfo(int32_t userId) {
  pthread_rwlock_rdlock(&rwlock_);
  auto itr = idCoinbaseInfos_.find(userId);
  pthread_rwlock_unlock(&rwlock_);

  if (itr != idCoinbaseInfos_.end()) {
    return itr->second;
  }
  return "";  // not found
}

int32_t UserInfo::incrementalUpdateUsers() {
  //
  // WARNING: The API is incremental update, we use `?last_id=` to make sure
  //          always get the new data. Make sure you have use `last_id` in API.
  //
  const string url = Strings::Format("%s?last_id=%d&last_time=%" PRId64, apiUrl_.c_str(), lastMaxUserId_, lastTime_);
  string resp;
  if (!httpGET(url.c_str(), resp, 10000/* timeout ms */)) {
    LOG(ERROR) << "http get request user list fail, url: " << url;
    return -1;
  }

  JsonNode r;
  if (!JsonNode::parse(resp.c_str(), resp.c_str() + resp.length(), r)) {
    LOG(ERROR) << "decode json fail, json: " << resp;
    return -1;
  }
  if (r["data"].type() == Utilities::JS::type::Undefined) {
    LOG(ERROR) << "invalid data, should key->value, type: " << (int)r["data"].type();
    return -1;
  }
  JsonNode data = r["data"];

  auto vUser = data["users"].children();
  if (vUser->size() == 0) {
    return 0;
  }
  lastTime_ = data["time"].int64();

  pthread_rwlock_wrlock(&rwlock_);
  for (JsonNode &itr : *vUser) {

    string userName(itr.key_start(), itr.key_end() - itr.key_start());
    regularUserName(userName);

    if (itr.type() != Utilities::JS::type::Obj) {
      LOG(ERROR) << "invalid data, should key  - value" << std::endl;
      return -1;
    }

    int32 userId = itr["puid"].int32();
    string coinbaseInfo = itr["coinbase"].str();

    // resize coinbaseInfo to USER_DEFINED_COINBASE_SIZE bytes
    if (coinbaseInfo.size() > USER_DEFINED_COINBASE_SIZE) {
      coinbaseInfo.resize(USER_DEFINED_COINBASE_SIZE);
    } else {
      // padding '\x20' at both beginning and ending of coinbaseInfo
      int beginPaddingLen = (USER_DEFINED_COINBASE_SIZE - coinbaseInfo.size()) / 2;
      coinbaseInfo.insert(0, beginPaddingLen, '\x20');
      coinbaseInfo.resize(USER_DEFINED_COINBASE_SIZE, '\x20');
    }

    if (userId > lastMaxUserId_) {
      lastMaxUserId_ = userId;
    }
    nameIds_[userName] = userId;

    // get user's coinbase info
    LOG(INFO) << "user id: " << userId << ", coinbase info: " << coinbaseInfo;
    idCoinbaseInfos_[userId] = coinbaseInfo;

  }
  pthread_rwlock_unlock(&rwlock_);

  return vUser->size();
}

/////////////////// End of user defined coinbase enabled ///////////////////
#else
////////////////////// User defined coinbase disabled //////////////////////

int32_t UserInfo::incrementalUpdateUsers() {
  //
  // WARNING: The API is incremental update, we use `?last_id=` to make sure
  //          always get the new data. Make sure you have use `last_id` in API.
  //
  const string url = Strings::Format("%s?last_id=%d", apiUrl_.c_str(), lastMaxUserId_);
  string resp;
  if (!httpGET(url.c_str(), resp, 10000/* timeout ms */)) {
    LOG(ERROR) << "http get request user list fail, url: " << url;
    return -1;
  }

  JsonNode r;
  if (!JsonNode::parse(resp.c_str(), resp.c_str() + resp.length(), r)) {
    LOG(ERROR) << "decode json fail, json: " << resp;
    return -1;
  }
  if (r["data"].type() == Utilities::JS::type::Undefined) {
    LOG(ERROR) << "invalid data, should key->value, type: " << (int)r["data"].type();
    return -1;
  }
  auto vUser = r["data"].children();
  if (vUser->size() == 0) {
    return 0;
  }

  pthread_rwlock_wrlock(&rwlock_);
  for (const auto &itr : *vUser) {
    string userName(itr.key_start(), itr.key_end() - itr.key_start());
    regularUserName(userName);

    const int32_t userId   = itr.int32();
    if (userId > lastMaxUserId_) {
      lastMaxUserId_ = userId;
    }

    nameIds_.insert(std::make_pair(userName, userId));
  }
  pthread_rwlock_unlock(&rwlock_);

  return vUser->size();
}

/////////////////// End of user defined coinbase disabled ///////////////////
#endif

void UserInfo::runThreadUpdate() {
  const time_t updateInterval = 10;  // seconds
  time_t lastUpdateTime = time(nullptr);

  while (running_) {
    if (lastUpdateTime + updateInterval > time(nullptr)) {
      usleep(500000);  // 500ms
      continue;
    }

    int32_t res = incrementalUpdateUsers();
    lastUpdateTime = time(nullptr);

    if (res > 0)
      LOG(INFO) << "update users count: " << res;
  }
}

bool UserInfo::setupThreads() {
  //
  // get all user list, incremental update model.
  //
  // We use `offset` in incrementalUpdateUsers(), will keep update uitl no more
  // new users. Most of http API have timeout limit, so can't return lots of
  // data in one request.
  //
  while (1) {
    int32_t res = incrementalUpdateUsers();
    if (res == 0)
      break;

    if (res == -1) {
      LOG(ERROR) << "update user list failure";
      return false;
    }

    LOG(INFO) << "update users count: " << res;
  }

  threadUpdate_ = thread(&UserInfo::runThreadUpdate, this);
  threadInsertWorkerName_ = thread(&UserInfo::runThreadInsertWorkerName, this);
  return true;
}

void UserInfo::addWorker(const int32_t userId, const int64_t workerId,
                         const string &workerName, const string &minerAgent) {
  ScopeLock sl(workerNameLock_);

  // insert to Q
  workerNameQ_.push_back(WorkerName());
  workerNameQ_.rbegin()->userId_   = userId;
  workerNameQ_.rbegin()->workerId_ = workerId;

  // worker name
  snprintf(workerNameQ_.rbegin()->workerName_,
           sizeof(workerNameQ_.rbegin()->workerName_),
           "%s", workerName.c_str());
  // miner agent
  snprintf(workerNameQ_.rbegin()->minerAgent_,
           sizeof(workerNameQ_.rbegin()->minerAgent_),
           "%s", minerAgent.c_str());
}

void UserInfo::runThreadInsertWorkerName() {
  while (running_) {
    if (insertWorkerName() > 0) {
      continue;
    }
    sleep(1);
  }
}

int32_t UserInfo::insertWorkerName() {
  std::deque<WorkerName>::iterator itr = workerNameQ_.end();
  {
    ScopeLock sl(workerNameLock_);
    if (workerNameQ_.size() == 0)
      return 0;
    itr = workerNameQ_.begin();
  }

  if (itr == workerNameQ_.end())
    return 0;


  // sent events to kafka: worker_update
  {
    string eventJson;
    eventJson = Strings::Format("{\"created_at\":\"%s\","
                                 "\"type\":\"worker_update\","
                                 "\"content\":{"
                                     "\"user_id\":%d,"
                                     "\"worker_id\":%" PRId64 ","
                                     "\"worker_name\":\"%s\","
                                     "\"miner_agent\":\"%s\""
                                "}}",
                                date("%F %T").c_str(),
                                itr->userId_,
                                itr->workerId_,
                                itr->workerName_,
                                itr->minerAgent_);
    server_->sendCommonEvents2Kafka(eventJson);
  }


  {
    ScopeLock sl(workerNameLock_);
    workerNameQ_.pop_front();
  }
  return 1;
}



////////////////////////////////// StratumJobEx ////////////////////////////////
StratumJobEx::StratumJobEx(StratumJob *sjob, bool isClean)
  : state_(0)
  , isClean_(isClean)
  , sjob_(sjob)
{
  assert(sjob != nullptr);
}

StratumJobEx::~StratumJobEx() {
  if (sjob_) {
    delete sjob_;
    sjob_ = nullptr;
  }
}

void StratumJobEx::markStale() {
  // 0: MINING, 1: STALE
  state_ = 1;
}

bool StratumJobEx::isStale() {
  // 0: MINING, 1: STALE
  return (state_ == 1);
}



///////////////////////////////////// StratumServer ///////////////////////////////////
StratumServer::StratumServer()
  : base_(nullptr), signal_event_(nullptr), listener_(nullptr)
  , kafkaProducerShareLog_(nullptr)
  , kafkaProducerSolvedShare_(nullptr)
  , kafkaProducerCommonEvents_(nullptr)
  , isEnableSimulator_(false)
  , isSubmitInvalidBlock_(false)
  , isDevModeEnable_(false)
  , devFixedDifficulty_(1.0)
#ifndef WORK_WITH_STRATUM_SWITCHER
  , sessionIDManager_(nullptr)
#endif
  , jobRepository_(nullptr)
  , userInfo_(nullptr)
  , serverId_(0)
{
}

StratumServer::~StratumServer() {
  if (signal_event_ != nullptr) {
    event_free(signal_event_);
  }
  if (listener_ != nullptr) {
    evconnlistener_free(listener_);
  }
  if (base_ != nullptr) {
    event_base_free(base_);
  }
  if (kafkaProducerShareLog_ != nullptr) {
    delete kafkaProducerShareLog_;
  }
  if (kafkaProducerSolvedShare_ != nullptr) {
    delete kafkaProducerSolvedShare_;
  }
  if (kafkaProducerCommonEvents_ != nullptr) {
    delete kafkaProducerCommonEvents_;
  }
  if (jobRepository_ != nullptr) {
    delete jobRepository_;
  }
  if (userInfo_ != nullptr) {
    delete userInfo_;
  }

#ifndef WORK_WITH_STRATUM_SWITCHER
  if (sessionIDManager_ != nullptr) {
    delete sessionIDManager_;
  }
#endif
}

void StratumServer::initZookeeper(const libconfig::Config &config) {
  if (!zk_) {
    zk_ = std::make_shared<Zookeeper>(config.lookup("zookeeper.brokers"));
  }
}

bool StratumServer::setup(const libconfig::Config &config) {
#ifdef WORK_WITH_STRATUM_SWITCHER
  LOG(INFO) << "WORK_WITH_STRATUM_SWITCHER enabled, miners can only connect to the sserver via a stratum switcher.";
#endif

  // ------------------- Development Options -------------------

  config.lookupValue("sserver.enable_simulator", isEnableSimulator_);
  if (isEnableSimulator_) {
    LOG(WARNING) << "[Dev Option] Simulator is enabled, all share will be accepted. "
                 << "This option should not be enabled in a production environment!";
  }

  config.lookupValue("sserver.enable_submit_invalid_block", isSubmitInvalidBlock_);
  if (isSubmitInvalidBlock_) {
    LOG(WARNING) << "[Dev Option] Submit invalid block is enabled, all shares will become solved shares. "
                 << "This option should not be enabled in a production environment!";
  }

  config.lookupValue("sserver.enable_dev_mode", isDevModeEnable_);
  if (isDevModeEnable_) {
    config.lookupValue("sserver.dev_fixed_difficulty", devFixedDifficulty_);
    LOG(WARNING) << "[Dev Option] Development mode is enabled with fixed difficulty: " << devFixedDifficulty_
                 << ". This option should not be enabled in a production environment!";
  }

  // ------------------- Diff Controller Options -------------------

  string defDiffStr = config.lookup("sserver.default_difficulty");
  uint64_t defaultDifficulty = stoull(defDiffStr, nullptr, 16);

  string maxDiffStr = config.lookup("sserver.max_difficulty");
  uint64_t maxDifficulty = stoull(maxDiffStr, nullptr, 16);

  string minDiffStr = config.lookup("sserver.min_difficulty");
  uint64_t minDifficulty = stoull(minDiffStr, nullptr, 16);

  uint32_t diffAdjustPeriod = 300;
  config.lookupValue("sserver.diff_adjust_period", diffAdjustPeriod);

  uint32_t shareAvgSeconds = 10; // default share interval time 10 seconds
  config.lookupValue("sserver.share_avg_seconds", shareAvgSeconds);

  if (0 == defaultDifficulty ||
      0 == maxDifficulty ||
      0 == minDifficulty ||
      0 == diffAdjustPeriod)
  {
    LOG(ERROR) << "difficulty settings are not expected: def=" << defaultDifficulty << ", min=" << minDifficulty << ", max=" << maxDifficulty << ", adjustPeriod=" << diffAdjustPeriod;
    return false;
  }

  if (diffAdjustPeriod < shareAvgSeconds) {
    LOG(ERROR) << "`diff_adjust_period` should not less than `share_avg_seconds`";
    return false;
  }

  defaultDifficultyController_ = make_shared<DiffController>(defaultDifficulty, maxDifficulty, minDifficulty, shareAvgSeconds, diffAdjustPeriod);

  // ------------------- Kafka Options -------------------

  string kafkaBrokers = config.lookup("kafka.brokers");

  kafkaProducerSolvedShare_ = new KafkaProducer(kafkaBrokers.c_str(),
                                                config.lookup("sserver.solved_share_topic").c_str(),
                                                RD_KAFKA_PARTITION_UA);
  kafkaProducerShareLog_ = new KafkaProducer(kafkaBrokers.c_str(),
                                             config.lookup("sserver.share_topic").c_str(),
                                             RD_KAFKA_PARTITION_UA);
  kafkaProducerCommonEvents_ = new KafkaProducer(kafkaBrokers.c_str(),
                                                 config.lookup("sserver.common_events_topic").c_str(),
                                                 RD_KAFKA_PARTITION_UA);

  // ------------------- Other Options -------------------

  string fileLastMiningNotifyTime;
  config.lookupValue("sserver.file_last_notify_time", fileLastMiningNotifyTime);
  
  uint32_t maxJobLifetime = 300;
  config.lookupValue("sserver.max_job_delay",    maxJobLifetime); // the old option name
  config.lookupValue("sserver.max_job_lifetime", maxJobLifetime); // the new name, overwrite the old if exist
  if (maxJobLifetime < 300) {
    LOG(WARNING) << "[Bad Option] sserver.max_job_lifetime (" << maxJobLifetime
                 << " seconds) is too short, recommended to be 300 seconds or longer.";
  }

  // job repository
  jobRepository_ = createJobRepository(kafkaBrokers.c_str(),
                                       config.lookup("sserver.job_topic").c_str(),
                                       fileLastMiningNotifyTime);
  jobRepository_->setMaxJobDelay(maxJobLifetime);
  if (!jobRepository_->setupThreadConsume()) {
    return false;
  }

  userInfo_ = new UserInfo(this, config);
  if (!userInfo_->setupThreads()) {
    return false;
  }

  // server id
  int serverId;
  config.lookupValue("sserver.id", serverId);
  if (serverId > 0xFF)
  {
    LOG(ERROR) << "invalid server id, range: [0, 255]";
    return false;
  }

  serverId_ = (uint8_t)serverId;
  if (serverId_ == 0) {
    // assign ID from zookeeper
    initZookeeper(config);
    serverId_ = zk_->getUniqIdUint8(config.lookup("sserver.zookeeper_lock_path"));
  }

#ifndef WORK_WITH_STRATUM_SWITCHER
  sessionIDManager_ = new SessionIDManagerT<24>(serverId_);
#endif

  // ------------------- Listen Options -------------------

  string listenIP = "0.0.0.0";
  config.lookupValue("sserver.ip", listenIP);

  int32_t listenPort = 3333;
  config.lookupValue("sserver.port", listenPort);

  // ------------------- Init Kafka -------------------

  // kafkaProducerShareLog_
  {
    map<string, string> options;
    // we could delay 'sharelog' in producer
    // 10000000 * sizeof(ShareBitcoin) ~= 480 MB
    options["queue.buffering.max.messages"] = "10000000";
    // send every second
    options["queue.buffering.max.ms"] = "1000";
    // 10000 * sizeof(ShareBitcoin) ~= 480 KB
    options["batch.num.messages"] = "10000";

    if (!kafkaProducerShareLog_->setup(&options)) {
      LOG(ERROR) << "kafka kafkaProducerShareLog_ setup failure";
      return false;
    }
    if (!kafkaProducerShareLog_->checkAlive()) {
      LOG(ERROR) << "kafka kafkaProducerShareLog_ is NOT alive";
      return false;
    }
  }

  // kafkaProducerSolvedShare_
  {
    map<string, string> options;
    // set to 1 (0 is an illegal value here), deliver msg as soon as possible.
    options["queue.buffering.max.ms"] = "1";
    if (!kafkaProducerSolvedShare_->setup(&options)) {
      LOG(ERROR) << "kafka kafkaProducerSolvedShare_ setup failure";
      return false;
    }
    if (!kafkaProducerSolvedShare_->checkAlive()) {
      LOG(ERROR) << "kafka kafkaProducerSolvedShare_ is NOT alive";
      return false;
    }
  }

  // kafkaProducerCommonEvents_
  {
    map<string, string> options;
    options["queue.buffering.max.messages"] = "500000";
    options["queue.buffering.max.ms"] = "1000";  // send every second
    options["batch.num.messages"]     = "10000";

    if (!kafkaProducerCommonEvents_->setup(&options)) {
      LOG(ERROR) << "kafka kafkaProducerCommonEvents_ setup failure";
      return false;
    }
    if (!kafkaProducerCommonEvents_->checkAlive()) {
      LOG(ERROR) << "kafka kafkaProducerCommonEvents_ is NOT alive";
      return false;
    }
  }

  // ------------------- TCP Listen -------------------

  base_ = event_base_new();
  if(!base_) {
    LOG(ERROR) << "server: cannot create base";
    return false;
  }

  memset(&sin_, 0, sizeof(sin_));
  sin_.sin_family = AF_INET;
  sin_.sin_port   = htons(listenPort);
  sin_.sin_addr.s_addr = htonl(INADDR_ANY);
  if (listenIP.empty() && inet_pton(AF_INET, listenIP.c_str(), &sin_.sin_addr) == 0) {
    LOG(ERROR) << "invalid ip: " << listenIP;
    return false;
  }

  listener_ = evconnlistener_new_bind(base_,
                                      StratumServer::listenerCallback,
                                      (void*)this,
                                      LEV_OPT_REUSEABLE|LEV_OPT_CLOSE_ON_FREE,
                                      -1, (struct sockaddr*)&sin_, sizeof(sin_));
  if(!listener_) {
    LOG(ERROR) << "cannot create listener: " << listenIP << ":" << listenPort;
    return false;
  }

  // ------------------- Derived Class Setup -------------------
  return setupInternal(config);
}

void StratumServer::run() {
  LOG(INFO) << "stratum server running";
  if(base_ != NULL) {
    //    event_base_loop(base_, EVLOOP_NONBLOCK);
    event_base_dispatch(base_);
  }
}

void StratumServer::stop() {
  LOG(INFO) << "stop stratum server";
  event_base_loopexit(base_, NULL);

  jobRepository_->stop();
  userInfo_->stop();
}

void StratumServer::sendMiningNotifyToAll(shared_ptr<StratumJobEx> exJobPtr) {
  //
  // http://www.sgi.com/tech/stl/Map.html
  //
  // Map has the important property that inserting a new element into a map
  // does not invalidate iterators that point to existing elements. Erasing
  // an element from a map also does not invalidate any iterators, except,
  // of course, for iterators that actually point to the element that is
  // being erased.
  //

  ScopeLock sl(connsLock_);
  auto itr = connections_.begin();
  while (itr != connections_.end()) {
    auto &conn = *itr;
    if (conn->isDead()) {
#ifndef WORK_WITH_STRATUM_SWITCHER
      sessionIDManager_->freeSessionId(conn->getSessionId());
#endif
      itr = connections_.erase(itr);
    } else {
      conn->sendMiningNotify(exJobPtr);
      ++itr;
    }
  }
}

void StratumServer::addConnection(unique_ptr<StratumSession> connection) {
  ScopeLock sl(connsLock_);
  connections_.insert(move(connection));
}

void StratumServer::removeConnection(StratumSession &connection) {
  //
  // if we are here, means the related evbuffer has already been locked.
  // don't lock connsLock_ in this function, it will cause deadlock.
  //
  connection.markAsDead();
}

void StratumServer::listenerCallback(struct evconnlistener* listener,
                              evutil_socket_t fd,
                              struct sockaddr *saddr,
                              int socklen, void* data)
{
  StratumServer *server = static_cast<StratumServer *>(data);
  struct event_base  *base = (struct event_base*)server->base_;
  struct bufferevent *bev;
  uint32_t sessionID = 0u;

#ifndef WORK_WITH_STRATUM_SWITCHER
  // can't alloc session Id
  if (server->sessionIDManager_->allocSessionId(&sessionID) == false) {
    close(fd);
    return;
  }
#endif

  bev = bufferevent_socket_new(base, fd, BEV_OPT_CLOSE_ON_FREE|BEV_OPT_THREADSAFE);
  if(bev == nullptr) {
    LOG(ERROR) << "error constructing bufferevent!";
    server->stop();
    return;
  }

  // create stratum session
  auto conn = server->createConnection(bev, saddr, sessionID);
  if (!conn->initialize())
  {
    return;
  }
  // set callback functions
  bufferevent_setcb(bev,
                    StratumServer::readCallback, nullptr,
                    StratumServer::eventCallback, conn.get());
  // By default, a newly created bufferevent has writing enabled.
  bufferevent_enable(bev, EV_READ|EV_WRITE);

  server->addConnection(move(conn));
}

void StratumServer::readCallback(struct bufferevent* bev, void *connection) {
  auto conn = static_cast<StratumSession *>(connection);
  conn->readBuf(bufferevent_get_input(bev));
}

void StratumServer::eventCallback(struct bufferevent* bev, short events,
                              void *connection) {
  auto conn = static_cast<StratumSession *>(connection);

  // should not be 'BEV_EVENT_CONNECTED'
  assert((events & BEV_EVENT_CONNECTED) != BEV_EVENT_CONNECTED);

  if (events & BEV_EVENT_EOF) {
    LOG(INFO) << "socket closed";
  }
  else if (events & BEV_EVENT_ERROR) {
    LOG(INFO) << "got an error on the socket: "
    << evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR());
  }
  else if (events & BEV_EVENT_TIMEOUT) {
    LOG(INFO) << "socket read/write timeout, events: " << events;
  }
  else {
    LOG(ERROR) << "unhandled socket events: " << events;
  }
  conn->getServer().removeConnection(*conn);
}



void StratumServer::sendShare2Kafka(const uint8_t *data, size_t len) {
  kafkaProducerShareLog_->produce(data, len);
}


void StratumServer::sendCommonEvents2Kafka(const string &message) {
  kafkaProducerCommonEvents_->produce(message.data(), message.size());
}
