#include <assert.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/eventfd.h>


#include <Log.h>
#include <Poller.h>
#include <Socket.h>
#include <EventLoop.h>
#include <Timestamp.h>
#include <Thread.h>
#include <Handle.h>
#include <Events.h>
#include <MutexLocker.h>


#include <boost/bind.hpp>
#include <boost/core/ignore_unused.hpp>



int EventLoop::count_ = 0;

EventLoop::EventLoop() :
  poll_(NULL),
  pollDelayTime_(10000),
  revents_(),
  handles_(),
  updates_(),
  mutex_(),
  cond_(mutex_),
  tid_(gettid()),
  wakeUpFd_(createEventfd()),
  wakeUpHandle_(new Handle(this, Events(wakeUpFd_, kReadEvent))),
  functors_(),
  runningFunctors_(false),
  quit_(false) {

  poll_.reset(new Poller);
  assert(poll_);
  count_++;

  wakeUpHandle_->setHandleRead(boost::bind(&EventLoop::handWakeUp, this, _1, _2));
  addHandle(wakeUpHandle_);
}

int EventLoop::getCount() const {
  return count_;
}

bool EventLoop::updateHandle(SHandlePtr handle) {
  LOG_TRACE("updateHandle");
  assert(handle->getLoop() == this);
  MutexLocker lock(mutex_);(void)lock;
  //It'll insert() fail when the key is same.
  updates_[handle->fd()] = handle;
  return true;
}

bool EventLoop::addHandle(HandlePtr handle) {
  assert(handle->getState() == Handle::state::STATE_NEW);
  handle->setState(Handle::state::STATE_ADD);
  return updateHandle(std::shared_ptr<Handle>(handle));
}

//FIXME : mod by fd
bool EventLoop::modHandle(HandlePtr handle) {
    LOG_TRACE("modHandle");
    assert(handles_.find(handle->fd()) != handles_.end());
    SHandlePtr mod = handles_.find(handle->fd())->second;
    mod->setState(Handle::state::STATE_MOD);
    return updateHandle(mod);
}

// FIXME : del by fd
bool EventLoop::delHandle(HandlePtr handle) {
  LOG_TRACE("delHandle");
  assert(handles_.find(handle->fd()) != handles_.end());
  SHandlePtr del = handles_.find(handle->fd())->second;
  del->setState(Handle::state::STATE_DEL);
  return updateHandle(del);
}

bool EventLoop::updateHandles() {
  LOG_TRACE("updateHandles");
  MutexLocker lock(mutex_);(void)lock;
  for(auto cur = updates_.begin(); cur != updates_.end(); cur++) {
    SHandlePtr handle = cur->second;
    Events* event = handle->getEvent();

    if (handle->getState() == Handle::state::STATE_ADD) {
      LOG_TRACE("updateHandles::STATE_ADD");
      poll_->events_add_(event);
      handles_[cur->first] = handle;
      handle->setState(Handle::state::STATE_LOOP);
    } else if (handle->getState() == Handle::state::STATE_MOD) {
      LOG_TRACE("updateHandles::STATE_MOD");
      poll_->events_mod_(event);
      handles_[cur->first] = handle;
      handle->setState(Handle::state::STATE_LOOP);
    }else if (handle->getState() == Handle::state::STATE_DEL) {
      LOG_TRACE("updateHandles::STATE_DEL");
      poll_->events_del_(event);
      int n = handles_.erase(handle->fd());
      assert(n == 1);
    } else {
      assert(false);
    }
  }
  return true;
}

bool EventLoop::isInLoopOwnerThread() {
  return (gettid() == tid_);
}

void EventLoop::assertInOwnerThread() {
  assert(isInLoopOwnerThread());
}

void EventLoop::wakeUp() {
  uint64_t one = 1;
  ssize_t n = ::write(wakeUpFd_, &one, sizeof one);
  if (n != sizeof one) {
    //cout << "EventLoop::wakeup() writes " << n << " bytes instead of 8" << endl;
  }

}

void EventLoop::handWakeUp(Events event, Timestamp time) {
  boost::ignore_unused(event, time);
  uint64_t one = 1;
  ssize_t n = ::read(wakeUpFd_, &one, sizeof one);
  if (n != sizeof one){
    std::cout << "EventLoop::handleRead() reads " << n << " bytes instead of 8" << std::endl;
  }
}

int createEventfd() {
  int evtfd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (evtfd < 0) {
    LOG_SYSERR("Failed in eventfd");
    ::abort();
  }
  return evtfd;
}


void EventLoop::runInLoop(const Functor& cb) {
  if (isInLoopOwnerThread()) {
    cb();
  } else {
    queueInLoop(cb);
  }
}

void EventLoop::queueInLoop(const Functor& cb) {
  {
    MutexLocker lock(mutex_);
    functors_.push_back(cb);
  }

  if (!isInLoopOwnerThread() || runningFunctors_) {
    wakeUp();
  }
}

void EventLoop::runFunctors() {
  std::vector<Functor> functors;
  runningFunctors_ = true;

  {
    MutexLocker lock(mutex_);
    functors.swap(functors_);
  }

  for (size_t i = 0; i < functors.size(); ++i) {
    functors[i]();
  }
  runningFunctors_ = false;
}

void EventLoop::quit() {
  MutexLocker lock(mutex_);
  quit_ = true;
  if(!isInLoopOwnerThread()) {
    wakeUp();
  }
  boost::ignore_unused(lock);
}

bool EventLoop::loop() {
  assertInOwnerThread();
  Timestamp looptime;

  while (!quit_) {
    if (!updates_.empty()) {
      updateHandles();
    }

    updates_.clear();
    revents_.clear();

    //std::cout << "tid : " << gettid() << " handles num " << handles_.size() << std::endl;

    looptime = poll_->loop_(revents_, pollDelayTime_);

    for(std::vector<Events>::iterator iter = revents_.begin();
        iter != revents_.end(); iter++) {
      //handle will decreament reference after for end!
      std::map<int, SHandlePtr>::iterator finditer = handles_.find((*iter).getFd());
      if (finditer == handles_.end()) {
        continue;
      }
      SHandlePtr handle = finditer->second;
      if (handle->getState() != Handle::state::STATE_LOOP) {
        LOG_DEBUG("After Loop have handle with state STATE_LOOP! it is unnormal!");
        continue;
      }
      //handle revents
      handle->handleEvent(*iter, looptime);
    } //for

    assert(!runningFunctors_);
    runFunctors();
  } //while
  return true;
}

EventLoop::~EventLoop() {
}
