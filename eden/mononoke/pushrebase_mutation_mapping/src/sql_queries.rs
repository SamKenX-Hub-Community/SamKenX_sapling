/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This software may be used and distributed according to the terms of the
 * GNU General Public License version 2.
 */

use anyhow::Result;
use mononoke_types::{ChangesetId, RepositoryId};
use pushrebase_hook::PushrebaseHook;
use sql::{queries, Connection, Transaction};
use sql_construct::{SqlConstruct, SqlConstructFromMetadataDatabaseConfig};
use sql_ext::SqlConnections;
use tunables::tunables;

use crate::save_mapping_pushrebase_hook::SaveMappingPushrebaseHook;
use crate::{PushrebaseMutationMapping, PushrebaseMutationMappingEntry};

queries! {
    read SelectPrepushrebaseIds(
        repo_id: RepositoryId,
        successor_bcs_id: ChangesetId,
    ) -> (ChangesetId,) {
        "SELECT predecessor_bcs_id
        FROM pushrebase_mutation_mapping
        WHERE repo_id = {repo_id} AND successor_bcs_id = {successor_bcs_id}"
    }

    write InsertMappingEntries(values:(
        repo_id: RepositoryId,
        predecessor_bcs_id: ChangesetId,
        successor_bcs_id: ChangesetId,
    )) {
        insert_or_ignore,
       "{insert_or_ignore}
       INTO pushrebase_mutation_mapping
       (repo_id, predecessor_bcs_id, successor_bcs_id)
       VALUES {values}"
    }
}

pub async fn add_pushrebase_mapping(
    transaction: Transaction,
    entries: &[PushrebaseMutationMappingEntry],
) -> Result<Transaction> {
    let entries: Vec<_> = entries
        .iter()
        .map(
            |
                PushrebaseMutationMappingEntry {
                    repo_id,
                    predecessor_bcs_id,
                    successor_bcs_id,
                },
            | (repo_id, predecessor_bcs_id, successor_bcs_id),
        )
        .collect();

    let (transaction, _) =
        InsertMappingEntries::query_with_transaction(transaction, &entries).await?;

    Ok(transaction)
}

// This is only used in tests thus it is unnecessary to keep a SQL connection
// in the mapping. We can just pass the connection to the function.
pub async fn get_prepushrebase_ids(
    connection: &Connection,
    repo_id: RepositoryId,
    successor_bcs_id: ChangesetId,
) -> Result<Vec<ChangesetId>> {
    let rows = SelectPrepushrebaseIds::query(&connection, &repo_id, &successor_bcs_id).await?;

    Ok(rows.into_iter().map(|r| r.0).collect())
}

pub struct SqlPushrebaseMutationMapping {
    repo_id: RepositoryId,
}

impl SqlPushrebaseMutationMapping {
    pub fn new(repo_id: RepositoryId, _sql_conn: SqlPushrebaseMutationMappingConnection) -> Self {
        Self { repo_id }
    }
}

pub struct SqlPushrebaseMutationMappingConnection {}

impl SqlPushrebaseMutationMappingConnection {
    pub fn with_repo_id(self, repo_id: RepositoryId) -> SqlPushrebaseMutationMapping {
        SqlPushrebaseMutationMapping::new(repo_id, self)
    }
}

impl SqlConstruct for SqlPushrebaseMutationMappingConnection {
    const LABEL: &'static str = "pushrebase_mutation_mapping";

    const CREATION_QUERY: &'static str =
        include_str!("../schemas/sqlite-pushrebase-mutation-mapping.sql");

    // We don't need the connections because we never use them.
    // But we need SqlConstruct to get our SQL tables created in tests.
    fn from_sql_connections(_connections: SqlConnections) -> Self {
        Self {}
    }
}

impl SqlConstructFromMetadataDatabaseConfig for SqlPushrebaseMutationMappingConnection {}

impl PushrebaseMutationMapping for SqlPushrebaseMutationMapping {
    fn get_hook(&self) -> Option<Box<dyn PushrebaseHook>> {
        if tunables().get_disable_save_mapping_pushrebase_hook() {
            None
        } else {
            Some(SaveMappingPushrebaseHook::new(self.repo_id))
        }
    }
}
