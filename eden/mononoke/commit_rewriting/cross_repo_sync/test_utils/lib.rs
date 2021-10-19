/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This software may be used and distributed according to the terms of the
 * GNU General Public License version 2.
 */

#![deny(warnings)]

use ascii::AsciiString;

use anyhow::{format_err, Error};
use blobrepo::BlobRepo;
use blobrepo_hg::BlobRepoHg;
use blobstore::Loadable;
use bookmarks::{BookmarkName, BookmarkUpdateReason};
use commit_transformation::upload_commits;
use context::CoreContext;
use cross_repo_sync::{
    rewrite_commit,
    types::{Source, Target},
    update_mapping_with_version, CommitSyncContext, CommitSyncDataProvider, CommitSyncRepos,
    CommitSyncer, SyncData, Syncers,
};
use futures::compat::Future01CompatExt;
use live_commit_sync_config::{LiveCommitSyncConfig, TestLiveCommitSyncConfig};
use maplit::hashmap;
use megarepolib::{common::ChangesetArgs, perform_move};
use metaconfig_types::{
    CommitSyncConfig, CommitSyncConfigVersion, CommitSyncDirection,
    DefaultSmallToLargeCommitSyncPathAction, SmallRepoCommitSyncConfig,
};
use mononoke_types::RepositoryId;
use mononoke_types::{ChangesetId, DateTime, MPath};
use sql::rusqlite::Connection as SqliteConnection;
use sql_construct::SqlConstruct;
use std::{collections::HashMap, sync::Arc};
use synced_commit_mapping::{
    SqlSyncedCommitMapping, SyncedCommitMapping, SyncedCommitMappingEntry,
};
use test_repo_factory::TestRepoFactory;
use tests_utils::{bookmark, CreateCommitContext};

// Helper function that takes a root commit from source repo and rebases it on master bookmark
// in target repo
pub async fn rebase_root_on_master<M>(
    ctx: CoreContext,
    commit_syncer: &CommitSyncer<M>,
    source_bcs_id: ChangesetId,
) -> Result<ChangesetId, Error>
where
    M: SyncedCommitMapping + Clone + 'static,
{
    let bookmark_name = BookmarkName::new("master").unwrap();
    let source_bcs = source_bcs_id
        .load(&ctx, commit_syncer.get_source_repo().blobstore())
        .await
        .unwrap();
    if !source_bcs.parents().collect::<Vec<_>>().is_empty() {
        return Err(format_err!("not a root commit"));
    }

    let maybe_bookmark_val = commit_syncer
        .get_target_repo()
        .get_bonsai_bookmark(ctx.clone(), &bookmark_name)
        .await?;

    let source_repo = commit_syncer.get_source_repo();
    let target_repo = commit_syncer.get_target_repo();

    let bookmark_val = maybe_bookmark_val.ok_or(format_err!("master not found"))?;
    let source_bcs_mut = source_bcs.into_mut();
    let maybe_rewritten = {
        let map = HashMap::new();
        let mover = commit_syncer
            .get_mover_by_version(&CommitSyncConfigVersion("TEST_VERSION_NAME".to_string()))
            .await?;
        rewrite_commit(&ctx, source_bcs_mut, &map, mover, source_repo.clone()).await?
    };
    let mut target_bcs_mut = maybe_rewritten.unwrap();
    target_bcs_mut.parents = vec![bookmark_val];

    let target_bcs = target_bcs_mut.freeze()?;

    upload_commits(
        &ctx,
        vec![target_bcs.clone()],
        commit_syncer.get_source_repo(),
        commit_syncer.get_target_repo(),
    )
    .await?;

    let mut txn = target_repo.update_bookmark_transaction(ctx.clone());
    txn.force_set(
        &bookmark_name,
        target_bcs.get_changeset_id(),
        BookmarkUpdateReason::TestMove,
        None,
    )
    .unwrap();
    txn.commit().await.unwrap();

    let entry = SyncedCommitMappingEntry::new(
        target_repo.get_repoid(),
        target_bcs.get_changeset_id(),
        source_repo.get_repoid(),
        source_bcs_id,
        CommitSyncConfigVersion("TEST_VERSION_NAME".to_string()),
        commit_syncer.get_source_repo_type(),
    );
    commit_syncer
        .get_mapping()
        .add(ctx.clone(), entry)
        .compat()
        .await?;

    Ok(target_bcs.get_changeset_id())
}

fn identity_mover(p: &MPath) -> Result<Option<MPath>, Error> {
    Ok(Some(p.clone()))
}

pub async fn init_small_large_repo(
    ctx: &CoreContext,
) -> Result<(Syncers<SqlSyncedCommitMapping>, CommitSyncConfig), Error> {
    let sqlite_con = SqliteConnection::open_in_memory()?;
    sqlite_con.execute_batch(SqlSyncedCommitMapping::CREATION_QUERY)?;
    let mut factory = TestRepoFactory::with_sqlite_connection(sqlite_con)?;
    let megarepo: BlobRepo = factory.with_id(RepositoryId::new(1)).build()?;
    let mapping =
        SqlSyncedCommitMapping::from_sql_connections(factory.metadata_db().clone().into());
    let smallrepo: BlobRepo = factory.with_id(RepositoryId::new(0)).build()?;

    let repos = CommitSyncRepos::SmallToLarge {
        small_repo: smallrepo.clone(),
        large_repo: megarepo.clone(),
    };

    let current_version = CommitSyncConfigVersion("TEST_VERSION_NAME".to_string());
    let noop_version = CommitSyncConfigVersion("noop".to_string());
    let commit_sync_data_provider = CommitSyncDataProvider::test_new(
        current_version.clone(),
        Source(repos.get_source_repo().get_repoid()),
        Target(repos.get_target_repo().get_repoid()),
        hashmap! {
            noop_version.clone() => SyncData {
                mover: Arc::new(identity_mover),
                reverse_mover: Arc::new(identity_mover),
                bookmark_renamer: Arc::new(identity_renamer),
                reverse_bookmark_renamer: Arc::new(identity_renamer),
            },
            current_version.clone() => SyncData {
                mover: Arc::new(prefix_mover),
                reverse_mover: Arc::new(reverse_prefix_mover),
                bookmark_renamer: Arc::new(identity_renamer),
                reverse_bookmark_renamer: Arc::new(identity_renamer),
            }
        },
        vec![BookmarkName::new("master")?],
    );

    let small_to_large_commit_syncer = CommitSyncer::new_with_provider(
        ctx,
        mapping.clone(),
        repos.clone(),
        commit_sync_data_provider,
    );

    let repos = CommitSyncRepos::LargeToSmall {
        small_repo: smallrepo.clone(),
        large_repo: megarepo.clone(),
    };

    let commit_sync_data_provider = CommitSyncDataProvider::test_new(
        current_version.clone(),
        Source(repos.get_source_repo().get_repoid()),
        Target(repos.get_target_repo().get_repoid()),
        hashmap! {
            noop_version.clone() =>  SyncData {
                mover: Arc::new(identity_mover),
                reverse_mover: Arc::new(identity_mover),
                bookmark_renamer: Arc::new(identity_renamer),
                reverse_bookmark_renamer: Arc::new(identity_renamer),
            },
            current_version => SyncData {
                mover: Arc::new(reverse_prefix_mover),
                reverse_mover: Arc::new(prefix_mover),
                bookmark_renamer: Arc::new(identity_renamer),
                reverse_bookmark_renamer: Arc::new(identity_renamer),
            }
        },
        vec![BookmarkName::new("master")?],
    );

    let large_to_small_commit_syncer = CommitSyncer::new_with_provider(
        ctx,
        mapping.clone(),
        repos.clone(),
        commit_sync_data_provider,
    );

    let first_bcs_id = CreateCommitContext::new_root(&ctx, &smallrepo)
        .add_file("file", "content")
        .commit()
        .await?;
    let second_bcs_id = CreateCommitContext::new(&ctx, &smallrepo, vec![first_bcs_id])
        .add_file("file2", "content")
        .commit()
        .await?;

    small_to_large_commit_syncer
        .unsafe_always_rewrite_sync_commit(
            ctx,
            first_bcs_id,
            None, // parents override
            &noop_version,
            CommitSyncContext::Tests,
        )
        .await?;
    small_to_large_commit_syncer
        .unsafe_always_rewrite_sync_commit(
            ctx,
            second_bcs_id,
            None, // parents override
            &noop_version,
            CommitSyncContext::Tests,
        )
        .await?;
    bookmark(&ctx, &smallrepo, "premove")
        .set_to(second_bcs_id)
        .await?;
    bookmark(&ctx, &megarepo, "premove")
        .set_to(second_bcs_id)
        .await?;

    let move_cs_args = ChangesetArgs {
        author: "Author Authorov".to_string(),
        message: "move commit".to_string(),
        datetime: DateTime::from_rfc3339("2018-11-29T12:00:00.00Z").unwrap(),
        bookmark: None,
        mark_public: false,
    };
    let move_hg_cs = perform_move(
        &ctx,
        &megarepo,
        second_bcs_id,
        Arc::new(prefix_mover),
        move_cs_args,
    )
    .await?;

    let maybe_move_bcs_id = megarepo.get_bonsai_from_hg(ctx.clone(), move_hg_cs).await?;
    let move_bcs_id = maybe_move_bcs_id.unwrap();

    bookmark(&ctx, &megarepo, "megarepo_start")
        .set_to(move_bcs_id)
        .await?;

    bookmark(&ctx, &smallrepo, "megarepo_start")
        .set_to("premove")
        .await?;

    // Master commit in the small repo after "big move"
    let small_master_bcs_id = CreateCommitContext::new(&ctx, &smallrepo, vec![second_bcs_id])
        .add_file("file3", "content3")
        .commit()
        .await?;

    // Master commit in large repo after "big move"
    let large_master_bcs_id = CreateCommitContext::new(&ctx, &megarepo, vec![move_bcs_id])
        .add_file("prefix/file3", "content3")
        .commit()
        .await?;

    bookmark(&ctx, &smallrepo, "master")
        .set_to(small_master_bcs_id)
        .await?;
    bookmark(&ctx, &megarepo, "master")
        .set_to(large_master_bcs_id)
        .await?;

    update_mapping_with_version(
        &ctx,
        hashmap! { small_master_bcs_id => large_master_bcs_id},
        &small_to_large_commit_syncer,
        &small_to_large_commit_syncer
            .get_current_version(&ctx)
            .await?,
    )
    .await?;

    println!(
        "small master: {}, large master: {}",
        small_master_bcs_id, large_master_bcs_id
    );
    println!(
        "{:?}",
        small_to_large_commit_syncer
            .get_commit_sync_outcome(&ctx, small_master_bcs_id)
            .await?
    );

    Ok((
        Syncers {
            small_to_large: small_to_large_commit_syncer,
            large_to_small: large_to_small_commit_syncer,
        },
        base_commit_sync_config(&megarepo, &smallrepo),
    ))
}

pub fn base_commit_sync_config(large_repo: &BlobRepo, small_repo: &BlobRepo) -> CommitSyncConfig {
    let small_repo_sync_config = SmallRepoCommitSyncConfig {
        default_action: DefaultSmallToLargeCommitSyncPathAction::PrependPrefix(
            MPath::new("prefix").unwrap(),
        ),
        map: hashmap! {},
        bookmark_prefix: AsciiString::new(),
        direction: CommitSyncDirection::SmallToLarge,
    };
    CommitSyncConfig {
        large_repo_id: large_repo.get_repoid(),
        common_pushrebase_bookmarks: vec![],
        small_repos: hashmap! {
            small_repo.get_repoid() => small_repo_sync_config,
        },
        version_name: CommitSyncConfigVersion("TEST_VERSION_NAME".to_string()),
    }
}

fn identity_renamer(b: &BookmarkName) -> Option<BookmarkName> {
    Some(b.clone())
}

fn prefix_mover(v: &MPath) -> Result<Option<MPath>, Error> {
    let prefix = MPath::new("prefix").unwrap();
    Ok(Some(MPath::join(&prefix, v)))
}

fn reverse_prefix_mover(v: &MPath) -> Result<Option<MPath>, Error> {
    let prefix = MPath::new("prefix").unwrap();
    if prefix.is_prefix_of(v) {
        Ok(v.remove_prefix_component(&prefix))
    } else {
        Ok(None)
    }
}

pub fn get_live_commit_sync_config() -> Arc<dyn LiveCommitSyncConfig> {
    let (sync_config, source) = TestLiveCommitSyncConfig::new_with_source();

    let first_version = CommitSyncConfig {
        large_repo_id: RepositoryId::new(0),
        common_pushrebase_bookmarks: vec![],
        small_repos: hashmap! {
            RepositoryId::new(1) => get_small_repo_sync_config_1(),
        },
        version_name: CommitSyncConfigVersion("first_version".to_string()),
    };

    let second_version = CommitSyncConfig {
        large_repo_id: RepositoryId::new(0),
        common_pushrebase_bookmarks: vec![],
        small_repos: hashmap! {
            RepositoryId::new(1) => get_small_repo_sync_config_2(),
        },
        version_name: CommitSyncConfigVersion("second_version".to_string()),
    };

    source.add_config(first_version);
    source.add_config(second_version);

    Arc::new(sync_config)
}

fn get_small_repo_sync_config_1() -> SmallRepoCommitSyncConfig {
    SmallRepoCommitSyncConfig {
        default_action: DefaultSmallToLargeCommitSyncPathAction::PrependPrefix(
            MPath::new("prefix").unwrap(),
        ),
        map: hashmap! {},
        bookmark_prefix: AsciiString::from_ascii("small".to_string()).unwrap(),
        direction: CommitSyncDirection::SmallToLarge,
    }
}

fn get_small_repo_sync_config_2() -> SmallRepoCommitSyncConfig {
    SmallRepoCommitSyncConfig {
        default_action: DefaultSmallToLargeCommitSyncPathAction::PrependPrefix(
            MPath::new("prefix").unwrap(),
        ),
        map: hashmap! {
            MPath::new("special").unwrap() => MPath::new("special").unwrap(),
        },
        bookmark_prefix: AsciiString::from_ascii("small".to_string()).unwrap(),
        direction: CommitSyncDirection::SmallToLarge,
    }
}
