/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This software may be used and distributed according to the terms of the
 * GNU General Public License version 2.
 */

#include "eden/fs/inodes/EdenMount.h"

#include <boost/filesystem.hpp>
#include <folly/ExceptionWrapper.h>
#include <folly/FBString.h>
#include <folly/stop_watch.h>

#include <folly/chrono/Conv.h>
#include <folly/futures/Future.h>
#include <folly/io/async/EventBase.h>
#include <folly/logging/Logger.h>
#include <folly/logging/xlog.h>
#include <folly/system/ThreadName.h>
#include <gflags/gflags.h>

#include "eden/fs/config/EdenConfig.h"
#include "eden/fs/inodes/CheckoutContext.h"
#include "eden/fs/inodes/EdenDispatcherFactory.h"
#include "eden/fs/inodes/FileInode.h"
#include "eden/fs/inodes/InodeError.h"
#include "eden/fs/inodes/InodeMap.h"
#include "eden/fs/inodes/ServerState.h"
#include "eden/fs/inodes/TreeInode.h"
#include "eden/fs/inodes/TreePrefetchLease.h"
#include "eden/fs/model/Hash.h"
#include "eden/fs/model/Tree.h"
#include "eden/fs/model/git/GitIgnoreStack.h"
#include "eden/fs/model/git/TopLevelIgnores.h"
#include "eden/fs/nfs/NfsServer.h"
#include "eden/fs/service/PrettyPrinters.h"
#include "eden/fs/service/gen-cpp2/eden_types.h"
#include "eden/fs/store/BlobAccess.h"
#include "eden/fs/store/DiffCallback.h"
#include "eden/fs/store/DiffContext.h"
#include "eden/fs/store/ObjectStore.h"
#include "eden/fs/store/ScmStatusDiffCallback.h"
#include "eden/fs/store/StatsFetchContext.h"
#include "eden/fs/telemetry/StructuredLogger.h"
#include "eden/fs/utils/Bug.h"
#include "eden/fs/utils/Clock.h"
#include "eden/fs/utils/FaultInjector.h"
#include "eden/fs/utils/Future.h"
#include "eden/fs/utils/NfsSocket.h"
#include "eden/fs/utils/PathFuncs.h"
#include "eden/fs/utils/SpawnedProcess.h"
#include "eden/fs/utils/UnboundedQueueExecutor.h"

#ifdef _WIN32
#include "eden/fs/prjfs/PrjfsChannel.h"
#else
#include <folly/File.h>
#include "eden/fs/fuse/FuseChannel.h"
#include "eden/fs/fuse/privhelper/PrivHelper.h"
#include "eden/fs/inodes/InodeTable.h"
#endif

using apache::thrift::ResponseChannelRequest;
using folly::Future;
using folly::makeFuture;
using folly::Try;
using folly::Unit;
using std::make_unique;
using std::shared_ptr;

DEFINE_int32(fuseNumThreads, 16, "how many fuse dispatcher threads to spawn");
DEFINE_string(
    edenfsctlPath,
    "edenfsctl",
    "the path to the edenfsctl executable");

namespace facebook {
namespace eden {

#ifndef _WIN32
namespace {
// We used to play tricks and hard link the .eden directory
// into every tree, but the linux kernel doesn't seem to like
// hard linking directories.  Now we create a symlink that resolves
// to the .eden directory inode in the root.
// The name of that symlink is `this-dir`:
// .eden/this-dir -> /abs/path/to/mount/.eden
constexpr PathComponentPiece kDotEdenSymlinkName{"this-dir"_pc};
constexpr PathComponentPiece kNfsdSocketName{"nfsd.socket"_pc};
} // namespace
#endif

/**
 * Helper for computing unclean paths when changing parents
 *
 * This DiffCallback instance is used to compute the set
 * of unclean files before and after actions that change the
 * current commit hash of the mount point.
 */
class EdenMount::JournalDiffCallback : public DiffCallback {
 public:
  explicit JournalDiffCallback()
      : data_{folly::in_place, std::unordered_set<RelativePath>()} {}

  void ignoredFile(RelativePathPiece) override {}

  void addedFile(RelativePathPiece) override {}

  void removedFile(RelativePathPiece path) override {
    data_.wlock()->uncleanPaths.insert(path.copy());
  }

  void modifiedFile(RelativePathPiece path) override {
    data_.wlock()->uncleanPaths.insert(path.copy());
  }

  void diffError(RelativePathPiece path, const folly::exception_wrapper& ew)
      override {
    // TODO: figure out what we should do to notify the user, if anything.
    // perhaps we should just add this path to the list of unclean files?
    XLOG(WARNING) << "error computing journal diff data for " << path << ": "
                  << folly::exceptionStr(ew);
  }

  FOLLY_NODISCARD Future<StatsFetchContext> performDiff(
      EdenMount* mount,
      TreeInodePtr rootInode,
      std::shared_ptr<const Tree> rootTree) {
    auto diffContext = mount->createDiffContext(this);
    auto rawContext = diffContext.get();

    return rootInode
        ->diff(
            rawContext,
            RelativePathPiece{},
            std::move(rootTree),
            rawContext->getToplevelIgnore(),
            false)
        .thenValue([diffContext = std::move(diffContext), rootInode](
                       folly::Unit) { return diffContext->getFetchContext(); });
  }

  /** moves the Unclean Path information out of this diff callback instance,
   * rendering it invalid */
  std::unordered_set<RelativePath> stealUncleanPaths() {
    std::unordered_set<RelativePath> result;
    std::swap(result, data_.wlock()->uncleanPaths);

    return result;
  }

 private:
  struct Data {
    explicit Data(std::unordered_set<RelativePath>&& unclean)
        : uncleanPaths(std::move(unclean)) {}

    std::unordered_set<RelativePath> uncleanPaths;
  };
  folly::Synchronized<Data> data_;
};

constexpr int EdenMount::kMaxSymlinkChainDepth;
static constexpr folly::StringPiece kEdenStracePrefix = "eden.strace.";

// We compute this when the process is initialized, but stash a copy
// in each EdenMount.  We may in the future manage to propagate enough
// state across upgrades or restarts that we can preserve this, but
// as implemented today, a process restart will invalidate any cached
// mountGeneration that a client may be holding on to.
// We take the bottom 16-bits of the pid and 32-bits of the current
// time and shift them up, leaving 16 bits for a mount point generation
// number.
static const uint64_t globalProcessGeneration =
    (uint64_t(getpid()) << 48) | (uint64_t(time(nullptr)) << 16);

// Each time we create an EdenMount we bump this up and OR it together
// with the globalProcessGeneration to come up with a generation number
// for a given mount instance.
static std::atomic<uint16_t> mountGeneration{0};

std::shared_ptr<EdenMount> EdenMount::create(
    std::unique_ptr<CheckoutConfig> config,
    std::shared_ptr<ObjectStore> objectStore,
    std::shared_ptr<BlobCache> blobCache,
    std::shared_ptr<ServerState> serverState,
    std::unique_ptr<Journal> journal) {
  return std::shared_ptr<EdenMount>{
      new EdenMount{
          std::move(config),
          std::move(objectStore),
          std::move(blobCache),
          std::move(serverState),
          std::move(journal)},
      EdenMountDeleter{}};
}

EdenMount::EdenMount(
    std::unique_ptr<CheckoutConfig> checkoutConfig,
    std::shared_ptr<ObjectStore> objectStore,
    std::shared_ptr<BlobCache> blobCache,
    std::shared_ptr<ServerState> serverState,
    std::unique_ptr<Journal> journal)
    : checkoutConfig_{std::move(checkoutConfig)},
      serverState_{std::move(serverState)},
      inodeMap_{new InodeMap(this)},
      objectStore_{std::move(objectStore)},
      blobCache_{std::move(blobCache)},
      blobAccess_{objectStore_, blobCache_},
      overlay_{Overlay::create(
          checkoutConfig_->getOverlayPath(),
          checkoutConfig_->getCaseSensitive(),
          getOverlayType(),
          serverState_->getStructuredLogger())},
#ifndef _WIN32
      overlayFileAccess_{overlay_.get()},
#endif
      journal_{std::move(journal)},
      mountGeneration_{globalProcessGeneration | ++mountGeneration},
      straceLogger_{
          kEdenStracePrefix.str() + checkoutConfig_->getMountPath().value()},
      lastCheckoutTime_{EdenTimestamp{serverState_->getClock()->getRealtime()}},
      owner_{Owner{getuid(), getgid()}},
      clock_{serverState_->getClock()} {
}

Overlay::OverlayType EdenMount::getOverlayType() {
  if (checkoutConfig_->getEnableTreeOverlay()) {
    if (getEdenConfig()->unsafeInMemoryOverlay.getValue()) {
      return Overlay::OverlayType::TreeInMemory;
    }
    if (getEdenConfig()->overlaySynchronousMode.getValue() == "off") {
      return Overlay::OverlayType::TreeSynchronousOff;
    }
    return Overlay::OverlayType::Tree;
  } else {
    return Overlay::OverlayType::Legacy;
  }
}

FOLLY_NODISCARD folly::Future<folly::Unit> EdenMount::initialize(
    OverlayChecker::ProgressCallback&& progressCallback,
    const std::optional<SerializedInodeMap>& takeover) {
  transitionState(State::UNINITIALIZED, State::INITIALIZING);

  return serverState_->getFaultInjector()
      .checkAsync("mount", getPath().stringPiece())
      .via(getServerThreadPool().get())
      .thenValue([this, progressCallback = std::move(progressCallback)](
                     auto&&) mutable {
        auto parent = checkoutConfig_->getParentCommit();
        *parentCommit_.wlock() = parent;

        // Record the transition from no snapshot to the current snapshot in
        // the journal.  This also sets things up so that we can carry the
        // snapshot id forward through subsequent journal entries.
        journal_->recordHashUpdate(parent);

        // Initialize the overlay.
        // This must be performed before we do any operations that may
        // allocate inode numbers, including creating the root TreeInode.
        return overlay_->initialize(getPath(), std::move(progressCallback))
            .deferValue([parent](auto&&) { return parent; });
      })
      .thenValue(
          [this](RootId parent) { return createRootInode(std::move(parent)); })
      .thenValue([this, takeover](TreeInodePtr initTreeNode) {
        if (takeover) {
          inodeMap_->initializeFromTakeover(std::move(initTreeNode), *takeover);
        } else if (isWorkingCopyPersistent()) {
          inodeMap_->initializeFromOverlay(std::move(initTreeNode), *overlay_);
        } else {
          inodeMap_->initialize(std::move(initTreeNode));
        }

        // TODO: It would be nice if the .eden inode was created before
        // allocating inode numbers for the Tree's entries. This would give the
        // .eden directory inode number 2.
        return setupDotEden(getRootInode());
      })
      .thenTry([this](auto&& result) {
        if (result.hasException()) {
          transitionState(State::INITIALIZING, State::INIT_ERROR);
        } else {
          transitionState(State::INITIALIZING, State::INITIALIZED);
        }
        return std::move(result);
      });
}

folly::Future<TreeInodePtr> EdenMount::createRootInode(
    const RootId& parentCommit) {
  // Load the overlay, if present.
  auto rootOverlayDir = overlay_->loadOverlayDir(kRootNodeId);
  if (!rootOverlayDir.empty()) {
    // No hash is necessary because the root is always materialized.
    return TreeInodePtr::makeNew(this, std::move(rootOverlayDir), std::nullopt);
  }

  static auto context = ObjectFetchContext::getNullContextWithCauseDetail(
      "EdenMount::createRootInode");
  return objectStore_->getRootTree(parentCommit, *context)
      .thenValue([this](std::shared_ptr<const Tree> tree) {
        return TreeInodePtr::makeNew(this, std::move(tree));
      });
}

#ifndef _WIN32
namespace {
Future<Unit> ensureDotEdenSymlink(
    TreeInodePtr directory,
    PathComponent symlinkName,
    AbsolutePath symlinkTarget) {
  enum class Action {
    Nothing,
    CreateSymlink,
    UnlinkThenSymlink,
  };

  static auto context =
      ObjectFetchContext::getNullContextWithCauseDetail("ensureDotEdenSymlink");
  return directory->getOrLoadChild(symlinkName, *context)
      .thenTryInline([=](Try<InodePtr>&& result) -> Future<Action> {
        if (!result.hasValue()) {
          // If we failed to look up the file this generally means it
          // doesn't exist.
          // TODO: it would be nicer to actually check the exception to
          // confirm it is ENOENT.  However, if it was some other error the
          // symlink creation attempt below will just fail with some
          // additional details anyway.
          return Action::CreateSymlink;
        }

        auto fileInode = result->asFilePtrOrNull();
        if (!fileInode) {
          // Hmm, it's unexpected that we would have a directory here.
          // Just return for now, without trying to replace the directory.
          // We'll continue mounting the checkout, but this symlink won't be
          // set up.  This potentially could confuse applications that look
          // for it later.
          XLOG(ERR) << "error setting up .eden/" << symlinkName
                    << " symlink: a directory exists at this location";
          return Action::Nothing;
        }

        // If there is a regular file at this location, remove it then
        // create the symlink.
        if (dtype_t::Symlink != fileInode->getType()) {
          return Action::UnlinkThenSymlink;
        }

        // Check if the symlink already has the desired contents.
        return fileInode->readlink(*context, CacheHint::LikelyNeededAgain)
            .thenValue([=](std::string&& contents) {
              if (contents == symlinkTarget) {
                // The symlink already contains the desired contents.
                return Action::Nothing;
              }
              // Remove and re-create the symlink with the desired contents.
              return Action::UnlinkThenSymlink;
            });
      })
      .thenValueInline([=](Action action) -> Future<Unit> {
        switch (action) {
          case Action::Nothing:
            return folly::unit;
          case Action::CreateSymlink:
            directory->symlink(
                symlinkName,
                symlinkTarget.stringPiece(),
                InvalidationRequired::Yes);
            return folly::unit;
          case Action::UnlinkThenSymlink:
            return directory
                ->unlink(symlinkName, InvalidationRequired::Yes, *context)
                .thenValueInline([=](Unit&&) {
                  directory->symlink(
                      symlinkName,
                      symlinkTarget.stringPiece(),
                      InvalidationRequired::Yes);
                });
        }
        EDEN_BUG() << "unexpected action type when configuring .eden directory";
      })
      .thenError([symlinkName](folly::exception_wrapper&& ew) {
        // Log the error but don't propagate it up to our caller.
        // We'll continue mounting the checkout even if we encountered an
        // error setting up some of these symlinks.  There's not much else
        // we can try here, and it is better to let the user continue
        // mounting the checkout so that it isn't completely unusable.
        XLOG(ERR) << "error setting up .eden/" << symlinkName
                  << " symlink: " << ew.what();
      });
}
} // namespace
#endif

folly::Future<folly::Unit> EdenMount::setupDotEden(TreeInodePtr root) {
  // Set up the magic .eden dir
  static auto context =
      ObjectFetchContext::getNullContextWithCauseDetail("setupDotEden");
  return root->getOrLoadChildTree(PathComponentPiece{kDotEdenName}, *context)
      .thenTryInline([=](Try<TreeInodePtr>&& lookupResult) {
        TreeInodePtr dotEdenInode;
        if (lookupResult.hasValue()) {
          dotEdenInode = *lookupResult;
        } else {
          dotEdenInode = getRootInode()->mkdir(
              PathComponentPiece{kDotEdenName},
              0755,
              InvalidationRequired::Yes);
        }

        // Make sure all of the symlinks in the .eden directory exist and
        // have the correct contents.
        std::vector<Future<Unit>> futures;

#ifndef _WIN32
        futures.emplace_back(ensureDotEdenSymlink(
            dotEdenInode,
            kDotEdenSymlinkName.copy(),
            (checkoutConfig_->getMountPath() +
             PathComponentPiece{kDotEdenName})));
        futures.emplace_back(ensureDotEdenSymlink(
            dotEdenInode, "root"_pc.copy(), checkoutConfig_->getMountPath()));
        futures.emplace_back(ensureDotEdenSymlink(
            dotEdenInode, "socket"_pc.copy(), serverState_->getSocketPath()));
        futures.emplace_back(ensureDotEdenSymlink(
            dotEdenInode,
            "client"_pc.copy(),
            checkoutConfig_->getClientDirectory()));
#endif

        // Wait until we finish setting up all of the symlinks.
        // Use collectAll() since we want to wait for everything to complete,
        // even if one of them fails early.
        return folly::collectAllUnsafe(futures).thenValue([=](auto&&) {
          // Set the dotEdenInodeNumber_ as our final step.
          // We do this after all of the ensureDotEdenSymlink() calls have
          // finished, since the TreeInode code will refuse to allow any
          // modifications to the .eden directory once we have set
          // dotEdenInodeNumber_.
          dotEdenInodeNumber_ = dotEdenInode->getNodeId();
        });
      });
}

#ifndef _WIN32
FOLLY_NODISCARD folly::Future<folly::Unit> EdenMount::addBindMount(
    RelativePathPiece repoPath,
    AbsolutePathPiece targetPath,
    ObjectFetchContext& context) {
  auto absRepoPath = getPath() + repoPath;

  return this->ensureDirectoryExists(repoPath, context)
      .thenValue([this,
                  target = targetPath.copy(),
                  pathInMountDir = getPath() + repoPath](auto&&) {
        return serverState_->getPrivHelper()->bindMount(
            target.stringPiece(), pathInMountDir.stringPiece());
      });
}

FOLLY_NODISCARD folly::Future<folly::Unit> EdenMount::removeBindMount(
    RelativePathPiece repoPath) {
  auto absRepoPath = getPath() + repoPath;
  return serverState_->getPrivHelper()->bindUnMount(absRepoPath.stringPiece());
}
#endif // !_WIN32

folly::SemiFuture<Unit> EdenMount::performBindMounts() {
  auto mountPath = getPath();
  return folly::makeSemiFutureWith([argv =
                                        std::vector<std::string>{
                                            FLAGS_edenfsctlPath,
                                            "redirect",
                                            "fixup",
                                            "--mount",
                                            mountPath.c_str()}] {
           return SpawnedProcess(argv).future_wait();
         })
      .deferValue([mountPath](ProcessStatus returnCode) {
        if (returnCode.exitStatus() == 0) {
          return folly::unit;
        }
        throw std::runtime_error(folly::to<std::string>(
            "Failed to run `",
            FLAGS_edenfsctlPath,
            " redirect fixup --mount ",
            mountPath,
            "`: exited with status ",
            returnCode.str()));
      })
      .deferError([mountPath](folly::exception_wrapper err) {
        throw std::runtime_error(folly::to<std::string>(
            "Failed to run `",
            FLAGS_edenfsctlPath,
            " fixup --mount ",
            mountPath,
            "`: ",
            folly::exceptionStr(err)));
      });
}

EdenMount::~EdenMount() {}

bool EdenMount::tryToTransitionState(State expected, State newState) {
  return state_.compare_exchange_strong(
      expected, newState, std::memory_order_acq_rel);
}

void EdenMount::transitionState(State expected, State newState) {
  State found = expected;
  if (!state_.compare_exchange_strong(
          found, newState, std::memory_order_acq_rel)) {
    throw std::runtime_error(folly::to<std::string>(
        "unable to transition mount ",
        getPath(),
        " to state ",
        newState,
        ": expected to be in state ",
        expected,
        " but actually in ",
        found));
  }
}

void EdenMount::transitionToFuseInitializationErrorState() {
  auto oldState = State::STARTING;
  auto newState = State::FUSE_ERROR;
  if (!state_.compare_exchange_strong(
          oldState, newState, std::memory_order_acq_rel)) {
    switch (oldState) {
      case State::DESTROYING:
      case State::SHUTTING_DOWN:
      case State::SHUT_DOWN:
        break;

      case State::INIT_ERROR:
      case State::FUSE_ERROR:
      case State::INITIALIZED:
      case State::INITIALIZING:
      case State::RUNNING:
      case State::UNINITIALIZED:
        XLOG(ERR)
            << "FUSE initialization error occurred for an EdenMount in the unexpected "
            << oldState << " state";
        break;

      case State::STARTING:
        XLOG(FATAL)
            << "compare_exchange_strong failed when transitioning EdenMount's state from "
            << oldState << " to " << newState;
        break;
    }
  }
}

static void logStats(
    bool success,
    AbsolutePath path,
    const RootId& fromRootId,
    const RootId& toRootId,
    const FetchStatistics& fetchStats,
    std::string_view methodName) {
  XLOG(DBG1) << (success ? "" : "failed ") << methodName << " for " << path
             << " from " << fromRootId << " to " << toRootId << " accessed "
             << fetchStats.tree.accessCount << " trees ("
             << fetchStats.tree.cacheHitRate << "% chr), "
             << fetchStats.blob.accessCount << " blobs ("
             << fetchStats.blob.cacheHitRate << "% chr), and "
             << fetchStats.metadata.accessCount << " metadata ("
             << fetchStats.metadata.cacheHitRate << "% chr).";
}

static folly::StringPiece getCheckoutModeString(CheckoutMode checkoutMode) {
  switch (checkoutMode) {
    case CheckoutMode::DRY_RUN:
      return "dry_run";
    case CheckoutMode::NORMAL:
      return "normal";
    case CheckoutMode::FORCE:
      return "force";
  }
  return "<unknown>";
}

#ifndef _WIN32
namespace {
TreeEntryType toEdenTreeEntryType(facebook::eden::ObjectType objectType) {
  switch (objectType) {
    case facebook::eden::ObjectType::TREE:
      return TreeEntryType::TREE;
    case facebook::eden::ObjectType::REGULAR_FILE:
      return TreeEntryType::REGULAR_FILE;
    case facebook::eden::ObjectType::EXECUTABLE_FILE:
      return TreeEntryType::EXECUTABLE_FILE;
    case facebook::eden::ObjectType::SYMLINK:
      return TreeEntryType::SYMLINK;
  }
  throw std::runtime_error("unsupported root type");
}

} // namespace

folly::Future<SetPathObjectIdResultAndTimes> EdenMount::setPathObjectId(
    FOLLY_MAYBE_UNUSED RelativePathPiece path,
    FOLLY_MAYBE_UNUSED const RootId& rootId,
    FOLLY_MAYBE_UNUSED ObjectType objectType,
    FOLLY_MAYBE_UNUSED CheckoutMode checkoutMode,
    FOLLY_MAYBE_UNUSED ObjectFetchContext& context) {
  if (objectType == facebook::eden::ObjectType::SYMLINK) {
    throw std::runtime_error("setPathObjectId does not support symlink type");
  }

  const folly::stop_watch<> stopWatch;
  auto setPathObjectIdTime = std::make_shared<SetPathObjectIdTimes>();
  /**
   * In theory, an exclusive wlock should be issued, but
   * this is not efficent if many calls to this method ran in parallel.
   * So we use read lock instead assuming the contents of loaded rootId
   * objects are not weaving too much
   */
  auto oldParent = parentCommit_.rlock();
  setPathObjectIdTime->didAcquireParentsLock = stopWatch.elapsed();
  XLOG(DBG3) << "adding " << rootId << " to Eedn mount " << this->getPath()
             << " at path" << path << " on top of " << *oldParent;

  auto ctx = std::make_shared<CheckoutContext>(
      this, checkoutMode, std::nullopt, "setPathObjectId");

  /**
   * This will update the timestamp for the entire mount,
   * TODO(yipu) We should only update the timestamp for the
   * partial node so only affects its children.
   */
  setLastCheckoutTime(EdenTimestamp{clock_->getRealtime()});
  bool isTree = (objectType == facebook::eden::ObjectType::TREE);

  auto getTargetTreeInodeFuture = ensureDirectoryExists(
      isTree ? path : path.dirname(), ctx->getFetchContext());

  auto getRootTreeFuture = isTree
      ? objectStore_->getRootTree(rootId, ctx->getFetchContext())
      : objectStore_
            ->getTreeEntryForRootId(
                rootId,
                toEdenTreeEntryType(objectType),
                path.basename(),
                ctx->getFetchContext())
            .thenValue([](std::shared_ptr<TreeEntry> treeEntry) {
              return std::make_shared<Tree>(std::vector<TreeEntry>{*treeEntry});
            });

  return collectSafe(getTargetTreeInodeFuture, getRootTreeFuture)
      .thenValue([this, ctx, setPathObjectIdTime, stopWatch](
                     std::tuple<TreeInodePtr, shared_ptr<const Tree>> results) {
        setPathObjectIdTime->didLookupTreesOrGetInodeByPath =
            stopWatch.elapsed();
        auto [targetTreeInode, incomingTree] = results;
        targetTreeInode->unloadChildrenUnreferencedByFs();
        // TODO(@yipu): Remove rename lock
        ctx->start(this->acquireRenameLock());
        setPathObjectIdTime->didAcquireRenameLock = stopWatch.elapsed();
        return targetTreeInode->checkout(ctx.get(), nullptr, incomingTree);
      })
      .thenValue([ctx, setPathObjectIdTime, stopWatch, rootId](auto&&) {
        setPathObjectIdTime->didCheckout = stopWatch.elapsed();
        // Complete and save the new snapshot
        return ctx->finish(rootId);
      })
      .thenValue([ctx, setPathObjectIdTime, stopWatch](
                     std::vector<CheckoutConflict>&& conflicts) {
        setPathObjectIdTime->didFinish = stopWatch.elapsed();
        SetPathObjectIdResultAndTimes resultAndTimes;
        resultAndTimes.times = *setPathObjectIdTime;
        SetPathObjectIdResult result;
        result.conflicts_ref() = std::move(conflicts);
        resultAndTimes.result = std::move(result);
        return resultAndTimes;
      })
      .thenTry([this, ctx, oldParent = *oldParent, rootId](
                   Try<SetPathObjectIdResultAndTimes>&& resultAndTimes) {
        auto fetchStats = ctx->getFetchContext().computeStatistics();
        logStats(
            resultAndTimes.hasValue(),
            this->getPath(),
            oldParent,
            rootId,
            fetchStats,
            "setPathObjectId");
        return std::move(resultAndTimes);
      });
}
#endif // !_WIN32

void EdenMount::destroy() {
  auto oldState = state_.exchange(State::DESTROYING, std::memory_order_acq_rel);
  switch (oldState) {
    case State::UNINITIALIZED:
    case State::INITIALIZING: {
      // The root inode may still be null here if we failed to load the root
      // inode.  In this case just delete ourselves immediately since we don't
      // have any inodes to unload.  shutdownImpl() requires the root inode be
      // loaded.
      if (!getRootInode()) {
        delete this;
      } else {
        // Call shutdownImpl() to destroy all loaded inodes.
        shutdownImpl(/*doTakeover=*/false);
      }
      return;
    }
    case State::INITIALIZED:
    case State::RUNNING:
    case State::STARTING:
    case State::INIT_ERROR:
    case State::FUSE_ERROR: {
      // Call shutdownImpl() to destroy all loaded inodes.
      shutdownImpl(/*doTakeover=*/false);
      return;
    }
    case State::SHUTTING_DOWN:
      // Nothing else to do.  shutdown() will destroy us when it completes.
      return;
    case State::SHUT_DOWN:
      // We were already shut down, and can delete ourselves immediately.
      XLOG(DBG1) << "destroying shut-down EdenMount " << getPath();
      delete this;
      return;
    case State::DESTROYING:
      // Fall through to the error handling code below.
      break;
  }

  XLOG(FATAL) << "EdenMount::destroy() called on mount " << getPath()
              << " in unexpected state " << oldState;
}

folly::SemiFuture<SerializedInodeMap> EdenMount::shutdown(
    bool doTakeover,
    bool allowFuseNotStarted) {
  // shutdown() should only be called on mounts that have not yet reached
  // SHUTTING_DOWN or later states.  Confirm this is the case, and move to
  // SHUTTING_DOWN.
  if (!(allowFuseNotStarted &&
        (tryToTransitionState(State::UNINITIALIZED, State::SHUTTING_DOWN) ||
         tryToTransitionState(State::INITIALIZING, State::SHUTTING_DOWN) ||
         tryToTransitionState(State::INITIALIZED, State::SHUTTING_DOWN))) &&
      !tryToTransitionState(State::RUNNING, State::SHUTTING_DOWN) &&
      !tryToTransitionState(State::STARTING, State::SHUTTING_DOWN) &&
      !tryToTransitionState(State::INIT_ERROR, State::SHUTTING_DOWN) &&
      !tryToTransitionState(State::FUSE_ERROR, State::SHUTTING_DOWN)) {
    EDEN_BUG() << "attempted to call shutdown() on a non-running EdenMount: "
               << "state was " << getState();
  }
  return shutdownImpl(doTakeover);
}

folly::SemiFuture<SerializedInodeMap> EdenMount::shutdownImpl(bool doTakeover) {
  journal_->cancelAllSubscribers();
  XLOG(DBG1) << "beginning shutdown for EdenMount " << getPath();

  return inodeMap_->shutdown(doTakeover)
      .thenValue([this](SerializedInodeMap inodeMap) {
        XLOG(DBG1) << "shutdown complete for EdenMount " << getPath();
        // Close the Overlay object to make sure we have released its lock.
        // This is important during graceful restart to ensure that we have
        // released the lock before the new edenfs process begins to take over
        // the mount point.
        overlay_->close();
        XLOG(DBG1) << "successfully closed overlay at " << getPath();
        auto oldState =
            state_.exchange(State::SHUT_DOWN, std::memory_order_acq_rel);
        if (oldState == State::DESTROYING) {
          delete this;
        }
        return inodeMap;
      });
}

folly::Future<folly::Unit> EdenMount::unmount() {
  return folly::makeFutureWith([this] {
    auto mountingUnmountingState = mountingUnmountingState_.wlock();
    if (mountingUnmountingState->channelUnmountStarted()) {
      return mountingUnmountingState->channelUnmountPromise->getFuture();
    }
    mountingUnmountingState->channelUnmountPromise.emplace();
    if (!mountingUnmountingState->channelMountStarted()) {
      return folly::makeFuture();
    }
    auto mountFuture =
        mountingUnmountingState->channelMountPromise->getFuture();
    mountingUnmountingState.unlock();

    return std::move(mountFuture)
        .thenTry([this](Try<Unit>&& mountResult) {
          if (mountResult.hasException()) {
            return folly::makeFuture();
          }
#ifdef _WIN32
          return channel_->stop()
              .via(getServerThreadPool().get())
              .ensure([this] { channel_.reset(); });
#else
          if (getNfsdChannel() != nullptr) {
            return serverState_->getPrivHelper()->nfsUnmount(
                getPath().stringPiece());
          } else {
            return serverState_->getPrivHelper()->fuseUnmount(
                getPath().stringPiece());
          }
#endif
        })
        .thenTry([this](Try<Unit>&& result) noexcept -> folly::Future<Unit> {
          auto mountingUnmountingState = mountingUnmountingState_.wlock();
          XDCHECK(mountingUnmountingState->channelUnmountPromise.has_value());
          folly::SharedPromise<folly::Unit>* unsafeUnmountPromise =
              &*mountingUnmountingState->channelUnmountPromise;
          mountingUnmountingState.unlock();

          unsafeUnmountPromise->setTry(Try<Unit>{result});
          return folly::makeFuture<folly::Unit>(std::move(result));
        });
  });
}

const shared_ptr<UnboundedQueueExecutor>& EdenMount::getServerThreadPool()
    const {
  return serverState_->getThreadPool();
}

std::shared_ptr<const EdenConfig> EdenMount::getEdenConfig() const {
  return serverState_->getReloadableConfig().getEdenConfig();
}

#ifndef _WIN32
InodeMetadataTable* EdenMount::getInodeMetadataTable() const {
  return overlay_->getInodeMetadataTable();
}

FuseChannel* FOLLY_NULLABLE EdenMount::getFuseChannel() const {
  if (auto channel = std::get_if<EdenMount::FuseChannelVariant>(&channel_)) {
    return channel->get();
  }
  return nullptr;
}

Nfsd3* FOLLY_NULLABLE EdenMount::getNfsdChannel() const {
  if (auto channel = std::get_if<EdenMount::NfsdChannelVariant>(&channel_)) {
    return channel->get();
  }
  return nullptr;
}
#else
PrjfsChannel* FOLLY_NULLABLE EdenMount::getPrjfsChannel() const {
  return channel_.get();
}
#endif

ProcessAccessLog& EdenMount::getProcessAccessLog() const {
#ifdef _WIN32
  return getPrjfsChannel()->getProcessAccessLog();
#else
  return std::visit(
      [](auto&& channel) -> ProcessAccessLog& {
        using T = std::decay_t<decltype(channel)>;
        if constexpr (!std::is_same_v<T, std::monostate>) {
          return channel->getProcessAccessLog();
        } else {
          EDEN_BUG() << "EdenMount::channel_ is not constructed.";
        }
      },
      channel_);
#endif
}

const AbsolutePath& EdenMount::getPath() const {
  return checkoutConfig_->getMountPath();
}

EdenStats* EdenMount::getStats() const {
  return &serverState_->getStats();
}

TreeInodePtr EdenMount::getRootInode() const {
  return inodeMap_->getRootInode();
}

folly::Future<std::shared_ptr<const Tree>> EdenMount::getRootTree() const {
  static auto context = ObjectFetchContext::getNullContextWithCauseDetail(
      "EdenMount::getRootTree");
  RootId commitHash = *parentCommit_.rlock();
  return objectStore_->getRootTree(commitHash, *context);
}

#ifndef _WIN32
InodeNumber EdenMount::getDotEdenInodeNumber() const {
  return dotEdenInodeNumber_;
}

#endif // !_WIN32

Future<InodePtr> EdenMount::getInode(
    RelativePathPiece path,
    ObjectFetchContext& context) const {
  return inodeMap_->getRootInode()->getChildRecursive(path, context);
}

folly::Future<std::string> EdenMount::loadFileContentsFromPath(
    ObjectFetchContext& fetchContext,
    RelativePathPiece path,
    CacheHint cacheHint) const {
  return getInode(path, fetchContext)
      .thenValue([this, &fetchContext, cacheHint](InodePtr fileInodePtr) {
        return loadFileContents(fetchContext, fileInodePtr, cacheHint);
      });
}

folly::Future<std::string> EdenMount::loadFileContents(
    ObjectFetchContext& fetchContext,
    InodePtr fileInodePtr,
    CacheHint cacheHint) const {
  const auto fileInode = fileInodePtr.asFileOrNull();
  if (!fileInode) {
    XLOG(WARNING) << "loadFile() invoked with a non-file inode: "
                  << fileInodePtr->getLogPath();
    return makeFuture<std::string>(InodeError(EISDIR, fileInodePtr));
  }

#ifndef _WIN32
  if (dtype_t::Symlink == fileInodePtr->getType()) {
    return resolveSymlink(fetchContext, fileInodePtr, cacheHint)
        .thenValue(
            [this, &fetchContext, cacheHint](
                InodePtr pResolved) mutable -> folly::Future<std::string> {
              // Note: infinite recursion is not a concern because
              // resolveSymlink() can not return a symlink
              return loadFileContents(fetchContext, pResolved, cacheHint);
            });
  }
#endif

  return fileInode->readAll(fetchContext, cacheHint);
}

#ifndef _WIN32
folly::Future<InodePtr> EdenMount::resolveSymlink(
    ObjectFetchContext& fetchContext,
    InodePtr pInode,
    CacheHint cacheHint) const {
  auto pathOptional = pInode->getPath();
  if (!pathOptional) {
    return makeFuture<InodePtr>(InodeError(ENOENT, pInode));
  }
  XLOG(DBG7) << "pathOptional.value() = " << pathOptional.value();
  return resolveSymlinkImpl(
      fetchContext, pInode, std::move(pathOptional.value()), 0, cacheHint);
}

folly::Future<InodePtr> EdenMount::resolveSymlinkImpl(
    ObjectFetchContext& fetchContext,
    InodePtr pInode,
    RelativePath&& path,
    size_t depth,
    CacheHint cacheHint) const {
  if (++depth > kMaxSymlinkChainDepth) { // max chain length exceeded
    return makeFuture<InodePtr>(InodeError(ELOOP, pInode));
  }

  // if pInode is not a symlink => it's already "resolved", so just return it
  if (dtype_t::Symlink != pInode->getType()) {
    return makeFuture(pInode);
  }

  const auto fileInode = pInode.asFileOrNull();
  if (!fileInode) {
    return EDEN_BUG_FUTURE(InodePtr)
        << "all symlink inodes must be FileInodes: " << pInode->getLogPath();
  }

  return fileInode->readlink(fetchContext, cacheHint)
      .thenValue([this,
                  &fetchContext,
                  pInode,
                  path = std::move(path),
                  depth,
                  cacheHint](std::string&& pointsTo) mutable {
        // normalized path to symlink target
        auto joinedExpected = joinAndNormalize(path.dirname(), pointsTo);
        if (joinedExpected.hasError()) {
          return makeFuture<InodePtr>(
              InodeError(joinedExpected.error(), pInode));
        }
        XLOG(DBG7) << "joinedExpected.value() = " << joinedExpected.value();
        // getting future below and doing .then on it are two separate
        // statements due to C++14 semantics (fixed in C++17) wherein RHS may
        // be executed before LHS, thus moving value of joinedExpected (in
        // RHS) before using it in LHS
        auto f = getInode(
            joinedExpected.value(),
            fetchContext); // get inode for symlink target
        return std::move(f).thenValue([this,
                                       &fetchContext,
                                       joinedPath =
                                           std::move(joinedExpected.value()),
                                       depth,
                                       cacheHint](InodePtr target) mutable {
          // follow the symlink chain recursively
          return resolveSymlinkImpl(
              fetchContext, target, std::move(joinedPath), depth, cacheHint);
        });
      });
}
#endif

folly::Future<CheckoutResult> EdenMount::checkout(
    const RootId& snapshotHash,
    std::optional<pid_t> clientPid,
    folly::StringPiece thriftMethodCaller,
    CheckoutMode checkoutMode) {
  const folly::stop_watch<> stopWatch;
  auto checkoutTimes = std::make_shared<CheckoutTimes>();

  // Hold the snapshot lock for the duration of the entire checkout operation.
  //
  // This prevents multiple checkout operations from running in parallel.

  auto parentLock = parentCommit_.wlock(std::chrono::milliseconds{500});

  if (!parentLock) {
    // We failed to get the lock, which generally means a checkout is in
    // progress.
    // Someone could be holding the lock in read-mode, but we normally only
    // hold the lock very briefly in read mode.  If we ever changed
    // EdenMount::diff() to hold the lock for the duration of the operation
    // we would need to update this code to account for that.
    //
    // TODO: Report the pid of the client that requested the first checkout
    // operation in this error
    return makeFuture<CheckoutResult>(newEdenError(
        EdenErrorType::CHECKOUT_IN_PROGRESS,
        "another checkout operation is still in progress"));
  }

  checkoutTimes->didAcquireParentsLock = stopWatch.elapsed();

  auto oldParent = *parentLock;
  auto ctx = std::make_shared<CheckoutContext>(
      this, std::move(parentLock), checkoutMode, clientPid, thriftMethodCaller);
  XLOG(DBG1) << "starting checkout for " << this->getPath() << ": " << oldParent
             << " to " << snapshotHash;

  // Update lastCheckoutTime_ before starting the checkout operation.
  // This ensures that any inode objects created once the checkout starts will
  // get the current checkout time, rather than the time from the previous
  // checkout
  setLastCheckoutTime(EdenTimestamp{clock_->getRealtime()});

  auto journalDiffCallback = std::make_shared<JournalDiffCallback>();
  return serverState_->getFaultInjector()
      .checkAsync("checkout", getPath().stringPiece())
      .via(getServerThreadPool().get())
      .thenValue([this, ctx, parent1Hash = oldParent, snapshotHash](auto&&) {
        auto fromTreeFuture =
            objectStore_->getRootTree(parent1Hash, ctx->getFetchContext());
        auto toTreeFuture =
            objectStore_->getRootTree(snapshotHash, ctx->getFetchContext());
        return collectSafe(fromTreeFuture, toTreeFuture);
      })
      .thenValue([this, ctx, checkoutTimes, stopWatch, journalDiffCallback](
                     std::tuple<shared_ptr<const Tree>, shared_ptr<const Tree>>
                         treeResults) {
        checkoutTimes->didLookupTrees = stopWatch.elapsed();
        // Call JournalDiffCallback::performDiff() to compute the changes
        // between the original working directory state and the source
        // tree state.
        //
        // If we are doing a dry-run update we aren't going to create a
        // journal entry, so we can skip this step entirely.
        if (ctx->isDryRun()) {
          return folly::makeFuture(treeResults);
        }

        auto& fromTree = std::get<0>(treeResults);
        return journalDiffCallback->performDiff(this, getRootInode(), fromTree)
            .thenValue([ctx, journalDiffCallback, treeResults](
                           const StatsFetchContext& diffFetchContext) {
              ctx->getFetchContext().merge(diffFetchContext);
              return treeResults;
            });
      })
      .thenValue([this, ctx, checkoutTimes, stopWatch](
                     std::tuple<shared_ptr<const Tree>, shared_ptr<const Tree>>
                         treeResults) {
        checkoutTimes->didDiff = stopWatch.elapsed();
        // Perform the requested checkout operation after the journal diff
        // completes.
        ctx->start(this->acquireRenameLock());

        checkoutTimes->didAcquireRenameLock = stopWatch.elapsed();

        // If a significant number of tree inodes are loaded or referenced
        // by FUSE, then checkout is slow, because Eden must precisely
        // manage changes to each one, as if the checkout was actually
        // creating and removing files in each directory. If a tree is
        // unloaded and unmodified, Eden can pretend the checkout
        // operation blew away the entire subtree and assigned new inode
        // numbers to everything under it, which is much cheaper.
        //
        // To make checkout faster, enumerate all loaded, unreferenced
        // inodes and unload them, allowing checkout to use the fast path.
        //
        // Note that this will not unload any inodes currently referenced
        // by FUSE, including the kernel's cache, so rapidly switching
        // between commits while working should not be materially
        // affected.
        //
        // On Windows, most of the above is also true, but instead of files
        // being referenced by the kernel, the files are actually on disk. All
        // the files on disk must also be present in the overlay, and thus the
        // checkout code will take care of doing the right invalidation for
        // these.
        this->getRootInode()->unloadChildrenUnreferencedByFs();

        auto rootInode = getRootInode();
        return serverState_->getFaultInjector()
            .checkAsync("inodeCheckout", getPath().stringPiece())
            .via(getServerThreadPool().get())
            .thenValue([ctx,
                        treeResults = std::move(treeResults),
                        rootInode = std::move(rootInode)](auto&&) mutable {
              auto& [fromTree, toTree] = treeResults;
              return rootInode->checkout(ctx.get(), fromTree, toTree);
            });
      })
      .thenValue([ctx, checkoutTimes, stopWatch, snapshotHash](auto&&) {
        checkoutTimes->didCheckout = stopWatch.elapsed();
        // Complete the checkout and save the new snapshot hash
        return ctx->finish(snapshotHash);
      })
      .thenValue(
          [this,
           ctx,
           checkoutTimes,
           stopWatch,
           oldParent,
           snapshotHash,
           journalDiffCallback](std::vector<CheckoutConflict>&& conflicts) {
            checkoutTimes->didFinish = stopWatch.elapsed();

            CheckoutResult result;
            result.times = *checkoutTimes;
            result.conflicts = std::move(conflicts);
            if (ctx->isDryRun()) {
              // This is a dry run, so all we need to do is tell the caller
              // about the conflicts: we should not modify any files or add
              // any entries to the journal.
              return result;
            }

            // Write a journal entry
            //
            // Note that we do not call journalDiffCallback->performDiff() a
            // second time here to compute the files that are now different
            // from the new state.  The checkout operation will only touch
            // files that are changed between fromTree and toTree.
            //
            // Any files that are unclean after the checkout operation must
            // have either been unclean before it started, or different
            // between the two trees.  Therefore the JournalDelta already
            // includes information that these files changed.
            auto uncleanPaths = journalDiffCallback->stealUncleanPaths();
            journal_->recordUncleanPaths(
                oldParent, snapshotHash, std::move(uncleanPaths));

            return result;
          })
      .thenTry([this, ctx, stopWatch, oldParent, snapshotHash, checkoutMode](
                   Try<CheckoutResult>&& result) {
        auto fetchStats = ctx->getFetchContext().computeStatistics();
        logStats(
            result.hasValue(),
            this->getPath(),
            oldParent,
            snapshotHash,
            fetchStats,
            "checkout");

        auto checkoutTimeInSeconds =
            std::chrono::duration<double>{stopWatch.elapsed()};
        auto event = FinishedCheckout{};
        event.mode = getCheckoutModeString(checkoutMode);
        event.duration = checkoutTimeInSeconds.count();
        event.success = result.hasValue();
        event.fetchedTrees = fetchStats.tree.fetchCount;
        event.fetchedBlobs = fetchStats.blob.fetchCount;
        // Don't log metadata fetches, because our backends don't yet support
        // fetching metadata directly. We expect tree fetches to eventually
        // return metadata for their entries.
        this->serverState_->getStructuredLogger()->logEvent(event);
        return std::move(result);
      });
}

#ifndef _WIN32
folly::Future<folly::Unit> EdenMount::chown(uid_t uid, gid_t gid) {
  // 1) Ensure that all future opens will by default provide this owner
  setOwner(uid, gid);

  // 2) Modify all uids/gids of files stored in the overlay
  auto metadata = getInodeMetadataTable();
  XDCHECK(metadata) << "Unexpected null Metadata Table";
  metadata->forEachModify([&](auto& /* unusued */, auto& record) {
    record.uid = uid;
    record.gid = gid;
  });

  // Note that any files being created at this point are not
  // guaranteed to have the requested uid/gid, but that racyness is
  // consistent with the behavior of chown

  // 3) Invalidate all inodes that the kernel holds a reference to
  auto inodesToInvalidate = getInodeMap()->getReferencedInodes();
  auto fuseChannel = getFuseChannel();
  XDCHECK(fuseChannel) << "Unexpected null Fuse Channel";
  fuseChannel->invalidateInodes(folly::range(inodesToInvalidate));

  return fuseChannel->flushInvalidations();
}
#endif

/*
During a diff, we have the possiblility of entering a non-mount aware code path.
Inside the non-mount aware code path, gitignore files still need to be honored.
In order to load a gitignore entry, a function pointer to
`EdenMount::loadFileContentsFromPath()` is passed through the `DiffContext` in
order to allow access the mount without creating a circular dependency. This
function starts at the root of the tree, and will follow the path and resolve
symlinks and will load inodes as needed in order to load the contents of the
file.
*/
std::unique_ptr<DiffContext> EdenMount::createDiffContext(
    DiffCallback* callback,
    bool listIgnored,
    ResponseChannelRequest* request) const {
  // We hold a reference to the root inode to ensure that
  // the EdenMount cannot be destroyed while the DiffContext
  // is still using it.
  auto loadContents = [this, rootInode = getRootInode()](
                          ObjectFetchContext& fetchContext,
                          RelativePathPiece path) {
    return loadFileContentsFromPath(
        fetchContext, path, CacheHint::LikelyNeededAgain);
  };
  return make_unique<DiffContext>(
      callback,
      listIgnored,
      getCheckoutConfig()->getCaseSensitive(),
      getObjectStore(),
      serverState_->getTopLevelIgnores(),
      std::move(loadContents),
      request);
}

Future<Unit> EdenMount::diff(DiffContext* ctxPtr, const RootId& commitHash)
    const {
  auto rootInode = getRootInode();
  return objectStore_->getRootTree(commitHash, ctxPtr->getFetchContext())
      .thenValue([ctxPtr, rootInode = std::move(rootInode)](
                     std::shared_ptr<const Tree>&& rootTree) {
        return rootInode->diff(
            ctxPtr,
            RelativePathPiece{},
            std::move(rootTree),
            ctxPtr->getToplevelIgnore(),
            false);
      });
}

Future<Unit> EdenMount::diff(
    DiffCallback* callback,
    const RootId& commitHash,
    bool listIgnored,
    bool enforceCurrentParent,
    ResponseChannelRequest* request) const {
  if (enforceCurrentParent) {
    auto parentInfo = parentCommit_.rlock(std::chrono::milliseconds{500});

    if (!parentInfo) {
      // We failed to get the lock, which generally means a checkout is in
      // progress.
      return makeFuture<Unit>(newEdenError(
          EdenErrorType::CHECKOUT_IN_PROGRESS,
          "cannot compute status while a checkout is currently in progress"));
    }

    if (*parentInfo != commitHash) {
      // Log this occurrence to Scuba
      getServerState()->getStructuredLogger()->logEvent(
          ParentMismatch{commitHash.value(), parentInfo->value()});
      return makeFuture<Unit>(newEdenError(
          EdenErrorType::OUT_OF_DATE_PARENT,
          "error computing status: requested parent commit is out-of-date: requested ",
          commitHash,
          ", but current parent commit is ",
          *parentInfo,
          ".\nTry running `eden doctor` to remediate"));
    }

    // TODO: Should we perhaps hold the parentInfo read-lock for the duration
    // of the status operation?  This would block new checkout operations from
    // starting until we have finished computing this status call.
  }

  // Create a DiffContext object for this diff operation.
  auto context = createDiffContext(callback, listIgnored, request);
  DiffContext* ctxPtr = context.get();

  // stateHolder() exists to ensure that the DiffContext and GitIgnoreStack
  // exists until the diff completes.
  auto stateHolder = [ctx = std::move(context)]() {};

  return diff(ctxPtr, commitHash).ensure(std::move(stateHolder));
}

folly::Future<std::unique_ptr<ScmStatus>> EdenMount::diff(
    const RootId& commitHash,
    bool listIgnored,
    bool enforceCurrentParent,
    ResponseChannelRequest* request) {
  auto callback = std::make_unique<ScmStatusDiffCallback>();
  auto callbackPtr = callback.get();
  return this
      ->diff(
          callbackPtr, commitHash, listIgnored, enforceCurrentParent, request)
      .thenValue([callback = std::move(callback)](auto&&) {
        return std::make_unique<ScmStatus>(callback->extractStatus());
      });
}

void EdenMount::resetParent(const RootId& parent) {
  // Hold the snapshot lock around the entire operation.
  auto parentLock = parentCommit_.wlock();
  auto oldParent = *parentLock;
  XLOG(DBG1) << "resetting snapshot for " << this->getPath() << " from "
             << oldParent << " to " << parent;

  // TODO: Maybe we should walk the inodes and see if we can dematerialize
  // some files using the new source control state.

  checkoutConfig_->setParentCommit(parent);
  *parentLock = parent;

  journal_->recordHashUpdate(oldParent, parent);
}

EdenTimestamp EdenMount::getLastCheckoutTime() const {
  static_assert(std::atomic<EdenTimestamp>::is_always_lock_free);
  return lastCheckoutTime_.load(std::memory_order_acquire);
}

void EdenMount::setLastCheckoutTime(EdenTimestamp time) {
  lastCheckoutTime_.store(time, std::memory_order_release);
}

RenameLock EdenMount::acquireRenameLock() {
  return RenameLock{this};
}

SharedRenameLock EdenMount::acquireSharedRenameLock() {
  return SharedRenameLock{this};
}

std::string EdenMount::getCounterName(CounterName name) {
  const auto& mountPath = getPath();
  const auto base = basename(mountPath.stringPiece());
  switch (name) {
    case CounterName::INODEMAP_LOADED:
      return folly::to<std::string>("inodemap.", base, ".loaded");
    case CounterName::INODEMAP_UNLOADED:
      return folly::to<std::string>("inodemap.", base, ".unloaded");
    case CounterName::JOURNAL_MEMORY:
      return folly::to<std::string>("journal.", base, ".memory");
    case CounterName::JOURNAL_ENTRIES:
      return folly::to<std::string>("journal.", base, ".count");
    case CounterName::JOURNAL_DURATION:
      return folly::to<std::string>("journal.", base, ".duration_secs");
    case CounterName::JOURNAL_MAX_FILES_ACCUMULATED:
      return folly::to<std::string>("journal.", base, ".files_accumulated.max");
  }
  EDEN_BUG() << "unknown counter name "
             << static_cast<std::underlying_type_t<CounterName>>(name);
}

folly::Future<TakeoverData::MountInfo> EdenMount::getChannelCompletionFuture() {
  return channelCompletionPromise_.getFuture();
}

#ifndef _WIN32
namespace {
std::unique_ptr<FuseChannel, FuseChannelDeleter> makeFuseChannel(
    EdenMount* mount,
    folly::File fuseFd) {
  auto edenConfig = mount->getEdenConfig();
  return std::unique_ptr<FuseChannel, FuseChannelDeleter>{new FuseChannel(
      std::move(fuseFd),
      mount->getPath(),
      FLAGS_fuseNumThreads,
      EdenDispatcherFactory::makeFuseDispatcher(mount),
      &mount->getStraceLogger(),
      mount->getServerState()->getProcessNameCache(),
      mount->getServerState()->getFsEventLogger(),
      std::chrono::duration_cast<folly::Duration>(
          edenConfig->fuseRequestTimeout.getValue()),
      mount->getServerState()->getNotifications(),
      mount->getCheckoutConfig()->getCaseSensitive(),
      mount->getCheckoutConfig()->getRequireUtf8Path(),
      edenConfig->fuseMaximumRequests.getValue())};
}
} // namespace
#endif

folly::Future<folly::Unit> EdenMount::channelMount(bool readOnly) {
  return folly::makeFutureWith([&] { return &beginMount(); })
      .thenValue([this, readOnly](folly::Promise<folly::Unit>* mountPromise) {
        AbsolutePath mountPath = getPath();
        auto edenConfig = getEdenConfig();
#ifdef _WIN32
        return folly::makeFutureWith([this,
                                      mountPath = std::move(mountPath),
                                      readOnly,
                                      edenConfig]() {
                 auto channel = std::make_unique<PrjfsChannel>(
                     mountPath,
                     EdenDispatcherFactory::makePrjfsDispatcher(this),
                     &getStraceLogger(),
                     serverState_->getProcessNameCache(),
                     std::chrono::duration_cast<folly::Duration>(
                         edenConfig->prjfsRequestTimeout.getValue()),
                     serverState_->getNotifications(),
                     getCheckoutConfig()->getRepoGuid());
                 channel->start(
                     readOnly,
                     edenConfig->prjfsUseNegativePathCaching.getValue());
                 return channel;
               })
            .thenTry([this, mountPromise](
                         Try<std::unique_ptr<PrjfsChannel>>&& channel) {
              if (channel.hasException()) {
                mountPromise->setException(channel.exception());
                return makeFuture<folly::Unit>(channel.exception());
              }

              // TODO(xavierd): similarly to the non-Windows code below, we
              // need to handle the case where mount was cancelled.

              mountPromise->setValue();
              channel_ = std::move(channel).value();
              return makeFuture(folly::unit);
            });
#else
        if (edenConfig->enableNfsServer.getValue() &&
            getCheckoutConfig()->getMountProtocol() == MountProtocol::NFS) {
          auto nfsServer = serverState_->getNfsServer();

          auto iosize = edenConfig->nfsIoSize.getValue();

          // Make sure that we are running on the EventBase while registering
          // the mount point.
          auto fut =
              via(nfsServer->getEventBase(),
                  [this, mountPath, nfsServer, iosize, edenConfig]() {
                    return nfsServer->registerMount(
                        mountPath,
                        getRootInode()->getNodeId(),
                        EdenDispatcherFactory::makeNfsDispatcher(this),
                        &getStraceLogger(),
                        serverState_->getProcessNameCache(),
                        serverState_->getFsEventLogger(),
                        std::chrono::duration_cast<folly::Duration>(
                            edenConfig->nfsRequestTimeout.getValue()),
                        serverState_->getNotifications(),
                        checkoutConfig_->getCaseSensitive(),
                        iosize);
                  });
          return std::move(fut).thenValue(
              [this,
               readOnly,
               iosize,
               mountPromise = std::move(mountPromise),
               mountPath = std::move(mountPath)](
                  NfsServer::NfsMountInfo mountInfo) mutable {
                auto [channel, mountdAddr] = std::move(mountInfo);

                std::optional<AbsolutePath> unixSocketPath;
                if (serverState_->getEdenConfig()->useUnixSocket.getValue()) {
                  unixSocketPath = getCheckoutConfig()->getClientDirectory() +
                      kNfsdSocketName;
                }
                channel->initialize(
                    makeNfsSocket(std::move(unixSocketPath)), false);

                return serverState_->getPrivHelper()
                    ->nfsMount(
                        mountPath.stringPiece(),
                        mountdAddr,
                        channel->getAddr(),
                        readOnly,
                        iosize)
                    .thenTry([this,
                              mountPromise = std::move(mountPromise),
                              channel = std::move(channel)](
                                 Try<folly::Unit>&& try_) mutable {
                      if (try_.hasException()) {
                        mountPromise->setException(try_.exception());
                        return folly::makeFuture<folly::Unit>(try_.exception());
                      }

                      mountPromise->setValue();
                      channel_ = std::move(channel);
                      return makeFuture(folly::unit);
                    });
              });
        } else {
          return serverState_->getPrivHelper()
              ->fuseMount(mountPath.stringPiece(), readOnly)
              .thenTry(
                  [mountPath, mountPromise, this](Try<folly::File>&& fuseDevice)
                      -> folly::Future<folly::Unit> {
                    if (fuseDevice.hasException()) {
                      mountPromise->setException(fuseDevice.exception());
                      return folly::makeFuture<folly::Unit>(
                          fuseDevice.exception());
                    }
                    if (mountingUnmountingState_.rlock()
                            ->channelUnmountStarted()) {
                      fuseDevice->close();
                      return serverState_->getPrivHelper()
                          ->fuseUnmount(mountPath.stringPiece())
                          .thenError(
                              folly::tag<std::exception>,
                              [](std::exception&& unmountError) {
                                // TODO(strager): Should we make
                                // EdenMount::unmount() also fail with the same
                                // exception?
                                XLOG(ERR)
                                    << "fuseMount was cancelled, but rollback (fuseUnmount) failed: "
                                    << unmountError.what();
                                throw std::move(unmountError);
                              })
                          .thenValue([mountPath, mountPromise](folly::Unit&&) {
                            auto error =
                                FuseDeviceUnmountedDuringInitialization{
                                    mountPath};
                            mountPromise->setException(error);
                            return folly::makeFuture<folly::Unit>(error);
                          });
                    }

                    mountPromise->setValue();
                    channel_ =
                        makeFuseChannel(this, std::move(fuseDevice).value());
                    return folly::makeFuture(folly::unit);
                  });
        }
#endif
      });
}

folly::Future<folly::Unit> EdenMount::startChannel(bool readOnly) {
  return folly::makeFutureWith([&]() {
    transitionState(
        /*expected=*/State::INITIALIZED, /*newState=*/State::STARTING);

    // Just in case the mount point directory doesn't exist,
    // automatically create it.
    boost::filesystem::path boostMountPath{getPath().value()};
    boost::filesystem::create_directories(boostMountPath);

    return channelMount(readOnly)
        .thenValue([this](auto&&) {
#ifdef _WIN32
          channelInitSuccessful(channel_->getStopFuture());
#else
          return std::visit(
              [this](auto&& variant) -> folly::Future<folly::Unit> {
                using T = std::decay_t<decltype(variant)>;

                if constexpr (std::
                                  is_same_v<T, EdenMount::FuseChannelVariant>) {
                  return variant->initialize().thenValue(
                      [this](FuseChannel::StopFuture&& fuseCompleteFuture) {
                        auto stopFuture =
                            std::move(fuseCompleteFuture)
                                .deferValue(
                                    [](FuseChannel::StopData&& stopData)
                                        -> EdenMount::ChannelStopData {
                                      return std::move(stopData);
                                    });
                        channelInitSuccessful(std::move(stopFuture));
                      });
                } else if constexpr (std::is_same_v<
                                         T,
                                         EdenMount::NfsdChannelVariant>) {
                  auto stopFuture = variant->getStopFuture().deferValue(
                      [](Nfsd3::StopData&& stopData)
                          -> EdenMount::ChannelStopData {
                        return std::move(stopData);
                      });
                  channelInitSuccessful(std::move(stopFuture));
                  return makeFuture(folly::unit);
                } else {
                  static_assert(std::is_same_v<T, std::monostate>);
                  return EDEN_BUG_FUTURE(folly::Unit)
                      << "EdenMount::channel_ is not constructed.";
                }
              },
              channel_);
#endif
        })
        .thenError([this](folly::exception_wrapper&& ew) {
          transitionToFuseInitializationErrorState();
          return makeFuture<folly::Unit>(std::move(ew));
        });
  });
}

folly::Promise<folly::Unit>& EdenMount::beginMount() {
  auto mountingUnmountingState = mountingUnmountingState_.wlock();
  if (mountingUnmountingState->channelMountPromise.has_value()) {
    EDEN_BUG() << __func__ << " unexpectedly called more than once";
  }
  if (mountingUnmountingState->channelUnmountStarted()) {
    throw EdenMountCancelled{};
  }
  mountingUnmountingState->channelMountPromise.emplace();
  // N.B. Return a reference to the lock-protected channelMountPromise member,
  // then release the lock. This is safe for two reasons:
  //
  // * *channelMountPromise will never be destructed (e.g. by calling
  //   std::optional<>::reset()) or reassigned. (channelMountPromise never goes
  //   from `has_value() == true` to `has_value() == false`.)
  //
  // * folly::Promise is self-synchronizing; getFuture() can be called
  //   concurrently with setValue()/setException().
  return *mountingUnmountingState->channelMountPromise;
}

void EdenMount::preparePostChannelCompletion(
    EdenMount::StopFuture&& channelCompleteFuture) {
  std::move(channelCompleteFuture)
      .via(getServerThreadPool().get())
      .thenValue(
          [this](FOLLY_MAYBE_UNUSED EdenMount::ChannelStopData&& stopData) {
#ifdef _WIN32
            inodeMap_->setUnmounted();
            std::vector<AbsolutePath> bindMounts;
            channelCompletionPromise_.setValue(TakeoverData::MountInfo(
                getPath(),
                checkoutConfig_->getClientDirectory(),
                bindMounts,
                folly::File{},
                SerializedInodeMap{} // placeholder
                ));
#else
            std::visit(
                [this](auto&& variant) {
                  using T = std::decay_t<decltype(variant)>;

                  if constexpr (std::is_same_v<T, EdenMount::FuseStopData>) {
                    // If the FUSE device is no longer valid then the mount
                    // point has been unmounted.
                    if (!variant.fuseDevice) {
                      inodeMap_->setUnmounted();
                    }

                    std::vector<AbsolutePath> bindMounts;

                    channelCompletionPromise_.setValue(TakeoverData::MountInfo(
                        getPath(),
                        checkoutConfig_->getClientDirectory(),
                        bindMounts,
                        std::move(variant.fuseDevice),
                        variant.fuseSettings,
                        SerializedInodeMap{} // placeholder
                        ));
                  } else {
                    static_assert(std::is_same_v<T, EdenMount::NfsdStopData>);
                    serverState_->getNfsServer()->unregisterMount(getPath());
                    inodeMap_->setUnmounted();
                    std::vector<AbsolutePath> bindMounts;
                    channelCompletionPromise_.setValue(TakeoverData::MountInfo(
                        getPath(),
                        checkoutConfig_->getClientDirectory(),
                        bindMounts,
                        // TODO(xavierd): the next 2 fields should be a variant
                        // too.
                        folly::File(),
                        fuse_init_out{},
                        SerializedInodeMap{} // placeholder
                        ));
                  }
                },
                stopData);
#endif
          })
      .thenError([this](folly::exception_wrapper&& ew) {
        XLOG(ERR) << "session complete with err: " << ew.what();
        channelCompletionPromise_.setException(std::move(ew));
      });
}

void EdenMount::channelInitSuccessful(
    EdenMount::StopFuture&& channelCompleteFuture) {
  // Try to transition to the RUNNING state.
  // This state transition could fail if shutdown() was called before we saw
  // the FUSE_INIT message from the kernel.
  transitionState(State::STARTING, State::RUNNING);
#ifndef _WIN32
  if (auto channel = std::get_if<NfsdChannelVariant>(&channel_)) {
    // Make sure that the Nfsd3 is destroyed in the EventBase that it was
    // created on. This is necessary as the various async sockets cannot be
    // used in multiple threads and can only be manipulated in the EventBase
    // they are attached to.
    preparePostChannelCompletion(
        std::move(channelCompleteFuture)
            .via(serverState_->getNfsServer()->getEventBase())
            .thenValue([this](EdenMount::ChannelStopData&& stopData) {
              channel_ = std::monostate{};
              return std::move(stopData);
            }));
  } else {
    preparePostChannelCompletion(std::move(channelCompleteFuture));
  }
#else
  preparePostChannelCompletion(std::move(channelCompleteFuture));
#endif
}

#ifndef _WIN32
void EdenMount::takeoverFuse(FuseChannelData takeoverData) {
  transitionState(State::INITIALIZED, State::STARTING);

  try {
    beginMount().setValue();

    auto channel = makeFuseChannel(this, std::move(takeoverData.fd));
    auto fuseCompleteFuture =
        channel->initializeFromTakeover(takeoverData.connInfo)
            .deferValue(
                [](FuseChannel::StopData&& stopData)
                    -> EdenMount::ChannelStopData {
                  return std::move(stopData);
                });
    channel_ = std::move(channel);
    channelInitSuccessful(std::move(fuseCompleteFuture));
  } catch (const std::exception&) {
    transitionToFuseInitializationErrorState();
    throw;
  }
}

InodeMetadata EdenMount::getInitialInodeMetadata(mode_t mode) const {
  auto owner = getOwner();
  return InodeMetadata{
      mode, owner.uid, owner.gid, InodeTimestamps{getLastCheckoutTime()}};
}
#endif

struct stat EdenMount::initStatData() const {
  struct stat st = {};

  auto owner = getOwner();
  st.st_uid = owner.uid;
  st.st_gid = owner.gid;
#ifndef _WIN32
  // We don't really use the block size for anything.
  // 4096 is fairly standard for many file systems.
  st.st_blksize = 4096;
#endif

  return st;
}

namespace {
Future<TreeInodePtr> ensureDirectoryExistsHelper(
    TreeInodePtr parent,
    PathComponentPiece childName,
    RelativePathPiece rest,
    ObjectFetchContext& context) {
  auto contents = parent->getContents().rlock();
  if (auto* child = folly::get_ptr(contents->entries, childName)) {
    if (!child->isDirectory()) {
      throw InodeError(EEXIST, parent, childName);
    }

    contents.unlock();

    if (rest.empty()) {
      return parent->getOrLoadChildTree(childName, context);
    }
    return parent->getOrLoadChildTree(childName, context)
        .thenValue([rest = RelativePath{rest}, &context](TreeInodePtr child) {
          auto [nextChildName, nextRest] = splitFirst(rest);
          return ensureDirectoryExistsHelper(
              child, nextChildName, nextRest, context);
        });
  }

  contents.unlock();
  TreeInodePtr child;
  try {
    child = parent->mkdir(childName, S_IFDIR | 0755, InvalidationRequired::Yes);
  } catch (std::system_error& e) {
    // If two threads are racing to create the subdirectory, that's fine,
    // just try again.
    if (e.code().value() == EEXIST) {
      return ensureDirectoryExistsHelper(parent, childName, rest, context);
    }
    throw;
  }
  if (rest.empty()) {
    return child;
  }
  auto [nextChildName, nextRest] = splitFirst(rest);
  return ensureDirectoryExistsHelper(child, nextChildName, nextRest, context);
}
} // namespace

Future<TreeInodePtr> EdenMount::ensureDirectoryExists(
    RelativePathPiece fromRoot,
    ObjectFetchContext& context) {
  if (fromRoot.empty()) {
    return getRootInode();
  }
  auto [childName, rest] = splitFirst(fromRoot);
  return ensureDirectoryExistsHelper(getRootInode(), childName, rest, context);
}

std::optional<TreePrefetchLease> EdenMount::tryStartTreePrefetch(
    TreeInodePtr treeInode,
    ObjectFetchContext& context) {
  auto config = serverState_->getEdenConfig(ConfigReloadBehavior::NoReload);
  auto maxTreePrefetches = config->maxTreePrefetches.getValue();
  auto numInProgress =
      numPrefetchesInProgress_.fetch_add(1, std::memory_order_acq_rel);
  if (numInProgress < maxTreePrefetches) {
    return TreePrefetchLease{std::move(treeInode), context};
  } else {
    numPrefetchesInProgress_.fetch_sub(1, std::memory_order_acq_rel);
    return std::nullopt;
  }
}

void EdenMount::treePrefetchFinished() noexcept {
  auto oldValue =
      numPrefetchesInProgress_.fetch_sub(1, std::memory_order_acq_rel);
  XDCHECK_NE(uint64_t{0}, oldValue);
}

bool EdenMount::MountingUnmountingState::channelMountStarted() const noexcept {
  return channelMountPromise.has_value();
}

bool EdenMount::MountingUnmountingState::channelUnmountStarted()
    const noexcept {
  return channelUnmountPromise.has_value();
}

EdenMountCancelled::EdenMountCancelled()
    : std::runtime_error{"EdenMount was unmounted during initialization"} {}

} // namespace eden
} // namespace facebook
