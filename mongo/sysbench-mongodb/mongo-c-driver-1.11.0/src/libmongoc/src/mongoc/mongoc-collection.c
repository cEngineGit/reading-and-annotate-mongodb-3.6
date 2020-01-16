/*
 * Copyright 2013 MongoDB, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


#include <stdio.h>

#include "mongoc-bulk-operation.h"
#include "mongoc-bulk-operation-private.h"
#include "mongoc-change-stream-private.h"
#include "mongoc-client-private.h"
#include "mongoc-find-and-modify-private.h"
#include "mongoc-find-and-modify.h"
#include "mongoc-collection.h"
#include "mongoc-collection-private.h"
#include "mongoc-cursor-private.h"
#include "mongoc-error.h"
#include "mongoc-index.h"
#include "mongoc-log.h"
#include "mongoc-trace-private.h"
#include "mongoc-read-concern-private.h"
#include "mongoc-write-concern-private.h"
#include "mongoc-read-prefs-private.h"
#include "mongoc-util-private.h"
#include "mongoc-write-command-private.h"
#include "mongoc-opts-private.h"

#undef MONGOC_LOG_DOMAIN
#define MONGOC_LOG_DOMAIN "collection"

static void
_mongoc_collection_write_command_execute (
   mongoc_write_command_t *command,
   const mongoc_collection_t *collection,
   const mongoc_write_concern_t *write_concern,
   mongoc_client_session_t *cs,
   mongoc_write_result_t *result)
{
   mongoc_server_stream_t *server_stream;

   ENTRY;

   server_stream = mongoc_cluster_stream_for_writes (
      &collection->client->cluster, cs, NULL, &result->error);

   if (!server_stream) {
      /* result->error has been filled out */
      EXIT;
   }

   _mongoc_write_command_execute (command,
                                  collection->client,
                                  server_stream,
                                  collection->db,
                                  collection->collection,
                                  write_concern,
                                  0 /* offset */,
                                  cs,
                                  result);

   mongoc_server_stream_cleanup (server_stream);

   EXIT;
}


static void
_mongoc_collection_write_command_execute_idl (
   mongoc_write_command_t *command,
   const mongoc_collection_t *collection,
   mongoc_crud_opts_t *crud,
   mongoc_write_result_t *result)
{
   mongoc_server_stream_t *server_stream;
   bson_t reply;

   ENTRY;

   server_stream =
      mongoc_cluster_stream_for_writes (&collection->client->cluster,
                                        crud->client_session,
                                        &reply,
                                        &result->error);

   if (!server_stream) {
      /* result->error and reply have been filled out */
      _mongoc_bson_array_copy_labels_to (&reply, &result->errorLabels);
      bson_destroy (&reply);
      EXIT;
   }

   if (_mongoc_client_session_in_txn (crud->client_session) &&
       crud->writeConcern) {
      bson_set_error (&result->error,
                      MONGOC_ERROR_COMMAND,
                      MONGOC_ERROR_COMMAND_INVALID_ARG,
                      "Cannot set write concern after starting transaction");
      mongoc_server_stream_cleanup (server_stream);
      EXIT;
   }

   if (!crud->writeConcern) {
      crud->writeConcern = collection->write_concern;
      crud->write_concern_owned = false;
   }

   _mongoc_write_command_execute_idl (command,
                                      collection->client,
                                      server_stream,
                                      collection->db,
                                      collection->collection,
                                      0 /* offset */,
                                      crud,
                                      result);

   mongoc_server_stream_cleanup (server_stream);

   EXIT;
}

/*
 *--------------------------------------------------------------------------
 *
 * _mongoc_collection_new --
 *
 *       INTERNAL API
 *
 *       Create a new mongoc_collection_t structure for the given client.
 *
 *       @client must remain valid during the lifetime of this structure.
 *       @db is the db name of the collection.
 *       @collection is the name of the collection.
 *       @read_prefs is the default read preferences to apply or NULL.
 *       @read_concern is the default read concern to apply or NULL.
 *       @write_concern is the default write concern to apply or NULL.
 *
 * Returns:
 *       A newly allocated mongoc_collection_t that should be freed with
 *       mongoc_collection_destroy().
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

mongoc_collection_t *
_mongoc_collection_new (mongoc_client_t *client,
                        const char *db,
                        const char *collection,
                        const mongoc_read_prefs_t *read_prefs,
                        const mongoc_read_concern_t *read_concern,
                        const mongoc_write_concern_t *write_concern)
{
   mongoc_collection_t *col;

   ENTRY;

   BSON_ASSERT (client);
   BSON_ASSERT (db);
   BSON_ASSERT (collection);

   col = (mongoc_collection_t *) bson_malloc0 (sizeof *col);
   col->client = client;
   col->write_concern = write_concern
                           ? mongoc_write_concern_copy (write_concern)
                           : mongoc_write_concern_new ();
   col->read_concern = read_concern ? mongoc_read_concern_copy (read_concern)
                                    : mongoc_read_concern_new ();
   col->read_prefs = read_prefs ? mongoc_read_prefs_copy (read_prefs)
                                : mongoc_read_prefs_new (MONGOC_READ_PRIMARY);

   bson_snprintf (col->ns, sizeof col->ns, "%s.%s", db, collection);
   bson_snprintf (col->db, sizeof col->db, "%s", db);
   bson_snprintf (col->collection, sizeof col->collection, "%s", collection);

   col->collectionlen = (uint32_t) strlen (col->collection);
   col->nslen = (uint32_t) strlen (col->ns);

   col->gle = NULL;

   RETURN (col);
}


/*
 *--------------------------------------------------------------------------
 *
 * mongoc_collection_destroy --
 *
 *       Release resources associated with @collection and frees the
 *       structure.
 *
 * Returns:
 *       None.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

void
mongoc_collection_destroy (mongoc_collection_t *collection) /* IN */
{
   ENTRY;

   if (!collection) {
      EXIT;
   }

   bson_clear (&collection->gle);

   if (collection->read_prefs) {
      mongoc_read_prefs_destroy (collection->read_prefs);
      collection->read_prefs = NULL;
   }

   if (collection->read_concern) {
      mongoc_read_concern_destroy (collection->read_concern);
      collection->read_concern = NULL;
   }

   if (collection->write_concern) {
      mongoc_write_concern_destroy (collection->write_concern);
      collection->write_concern = NULL;
   }

   bson_free (collection);

   EXIT;
}


/*
 *--------------------------------------------------------------------------
 *
 * mongoc_collection_copy --
 *
 *       Returns a copy of @collection that needs to be freed by calling
 *       mongoc_collection_destroy.
 *
 * Returns:
 *       A copy of this collection.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

mongoc_collection_t *
mongoc_collection_copy (mongoc_collection_t *collection) /* IN */
{
   ENTRY;

   BSON_ASSERT (collection);

   RETURN (_mongoc_collection_new (collection->client,
                                   collection->db,
                                   collection->collection,
                                   collection->read_prefs,
                                   collection->read_concern,
                                   collection->write_concern));
}


static bool
_make_agg_cmd (const char *coll,
               const bson_t *pipeline,
               const bson_t *opts,
               bson_t *command,
               bson_error_t *err)
{
   bson_iter_t iter;
   int32_t batch_size = 0;
   bson_t child;

   bson_init (command);
   BSON_APPEND_UTF8 (command, "aggregate", coll);
   /*
    * The following will allow @pipeline to be either an array of
    * items for the pipeline, or {"pipeline": [...]}.
    */
   if (bson_iter_init_find (&iter, pipeline, "pipeline") &&
       BSON_ITER_HOLDS_ARRAY (&iter)) {
      if (!bson_append_iter (command, "pipeline", 8, &iter)) {
         bson_set_error (err,
                         MONGOC_ERROR_COMMAND,
                         MONGOC_ERROR_COMMAND_INVALID_ARG,
                         "Failed to append \"pipeline\" to create command.");
         return false;
      }
   } else {
      BSON_APPEND_ARRAY (command, "pipeline", pipeline);
   }

   bson_append_document_begin (command, "cursor", 6, &child);
   if (opts && bson_iter_init_find (&iter, opts, "batchSize") &&
       BSON_ITER_HOLDS_NUMBER (&iter)) {
      batch_size = (int32_t) bson_iter_as_int64 (&iter);
      BSON_APPEND_INT32 (&child, "batchSize", batch_size);
   }
   bson_append_document_end (command, &child);
   return true;
}

/*
 *--------------------------------------------------------------------------
 *
 * mongoc_collection_aggregate --
 *
 *       Send an "aggregate" command to the MongoDB server.
 *
 *       This function will always return a new mongoc_cursor_t that should
 *       be freed with mongoc_cursor_destroy().
 *
 *       The cursor may fail once iterated upon, so check
 *       mongoc_cursor_error() if mongoc_cursor_next() returns false.
 *
 *       See http://docs.mongodb.org/manual/aggregation/ for more
 *       information on how to build aggregation pipelines.
 *
 * Parameters:
 *       @flags: bitwise or of mongoc_query_flags_t or 0.
 *       @pipeline: A bson_t containing the pipeline request. @pipeline
 *                  will be sent as an array type in the request.
 *       @options:  A bson_t containing aggregation options, such as
 *                  bypassDocumentValidation (used with $out pipeline),
 *                  maxTimeMS (declaring maximum server execution time) and
 *                  explain (return information on the processing of the
 *                  pipeline).
 *       @read_prefs: Optional read preferences for the command.
 *
 * Returns:
 *       A newly allocated mongoc_cursor_t that should be freed with
 *       mongoc_cursor_destroy().
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */


mongoc_cursor_t *
mongoc_collection_aggregate (mongoc_collection_t *collection,       /* IN */
                             mongoc_query_flags_t flags,            /* IN */
                             const bson_t *pipeline,                /* IN */
                             const bson_t *opts,                    /* IN */
                             const mongoc_read_prefs_t *read_prefs) /* IN */
{
   mongoc_server_stream_t *server_stream = NULL;
   bool has_out_key = false;
   bool has_read_concern;
   bool has_write_concern;
   bson_iter_t kiter;
   bson_iter_t ar;
   mongoc_cursor_t *cursor;
   uint32_t server_id;
   bson_iter_t iter;
   bson_t command;
   bson_t cursor_opts;
   bool slave_ok;
   bool created_command;
   bson_error_t create_cmd_err = {0};

   ENTRY;

   BSON_ASSERT (collection);
   BSON_ASSERT (pipeline);

   bson_init (&cursor_opts);
   _mongoc_cursor_flags_to_opts (flags, &cursor_opts, &slave_ok);
   if (opts) {
      bson_concat (&cursor_opts, opts);
   }

   created_command = _make_agg_cmd (
      collection->collection, pipeline, opts, &command, &create_cmd_err);
   cursor = _mongoc_cursor_cmd_new (collection->client,
                                    collection->ns,
                                    created_command ? &command : NULL,
                                    &cursor_opts,
                                    read_prefs,
                                    NULL /* read concern */);
   bson_destroy (&command);
   bson_destroy (&cursor_opts);

   if (!created_command) {
      /* copy error back to cursor. */
      memcpy (&cursor->error, &create_cmd_err, sizeof (bson_error_t));
      GOTO (done);
   }

   /* Get serverId from opts; if invalid set cursor err. _mongoc_cursor_cmd_new
    * has already done this, but we want a COMMAND error, not CURSOR, since that
    * has been the contract since serverId was first implemented. */
   if (!_mongoc_get_server_id_from_opts (opts,
                                         MONGOC_ERROR_COMMAND,
                                         MONGOC_ERROR_COMMAND_INVALID_ARG,
                                         &server_id,
                                         &cursor->error)) {
      GOTO (done);
   }

   if (mongoc_cursor_error (cursor, NULL)) {
      /* something else is wrong with opts */
      GOTO (done);
   }

   if (server_id) {
      /* don't use mongoc_cursor_set_hint, don't want special slaveok logic */
      cursor->server_id = server_id;
   }

   /* server id isn't enough. ensure we're connected & know wire version */
   server_stream = _mongoc_cursor_fetch_stream (cursor);
   if (!server_stream) {
      GOTO (done);
   }

   if (!read_prefs && !server_id) {
      mongoc_read_prefs_destroy (cursor->read_prefs);
      cursor->read_prefs = mongoc_read_prefs_copy (collection->read_prefs);
   }

   if (!_mongoc_read_prefs_validate (cursor->read_prefs, &cursor->error)) {
      GOTO (done);
   }

   if (bson_iter_init_find (&iter, pipeline, "pipeline") &&
       BSON_ITER_HOLDS_ARRAY (&iter) && bson_iter_recurse (&iter, &ar)) {
      while (bson_iter_next (&ar)) {
         if (BSON_ITER_HOLDS_DOCUMENT (&ar) &&
             bson_iter_recurse (&ar, &kiter)) {
            has_out_key |= bson_iter_find (&kiter, "$out");
         }
      }
   }

   has_write_concern = bson_has_field (&cursor->opts, "writeConcern");
   if (has_write_concern &&
       server_stream->sd->max_wire_version < WIRE_VERSION_CMD_WRITE_CONCERN) {
      bson_set_error (&cursor->error,
                      MONGOC_ERROR_COMMAND,
                      MONGOC_ERROR_PROTOCOL_BAD_WIRE_VERSION,
                      "\"aggregate\" with \"$out\" does not support "
                      "writeConcern with wire version %d, wire version %d is "
                      "required",
                      server_stream->sd->max_wire_version,
                      WIRE_VERSION_CMD_WRITE_CONCERN);
      GOTO (done);
   }

   /* Only inherit WriteConcern when for aggregate with $out */
   if (!bson_has_field (&cursor->opts, "writeConcern") && has_out_key) {
      mongoc_write_concern_destroy (cursor->write_concern);
      cursor->write_concern = mongoc_write_concern_copy (
         mongoc_collection_get_write_concern (collection));
   }

   has_read_concern = bson_has_field (&cursor->opts, "readConcern");
   if (!has_read_concern) {
      mongoc_read_concern_destroy (cursor->read_concern);
      cursor->read_concern = mongoc_read_concern_copy (
         mongoc_collection_get_read_concern (collection));
   }

done:
   mongoc_server_stream_cleanup (server_stream); /* null ok */

   /* we always return the cursor, even if it fails; users can detect the
    * failure on performing a cursor operation. see CDRIVER-880. */
   RETURN (cursor);
}


/*
 *--------------------------------------------------------------------------
 *
 * mongoc_collection_find --
 *
 *       DEPRECATED: use mongoc_collection_find_with_opts.
 *
 *       Performs a query against the configured MongoDB server. If @read_prefs
 *       is provided, it will be used to locate a MongoDB node in the cluster
 *       to deliver the query to.
 *
 *       @flags may be bitwise-or'd flags or MONGOC_QUERY_NONE.
 *
 *       @skip may contain the number of documents to skip before returning the
 *       matching document.
 *
 *       @limit may contain the maximum number of documents that may be
 *       returned.
 *
 *       This function will always return a cursor, with the exception of
 *       invalid API use.
 *
 * Parameters:
 *       @collection: A mongoc_collection_t.
 *       @flags: A bitwise or of mongoc_query_flags_t.
 *       @skip: The number of documents to skip.
 *       @limit: The maximum number of items.
 *       @batch_size: The batch size
 *       @query: The query to locate matching documents.
 *       @fields: The fields to return, or NULL for all fields.
 *       @read_prefs: Read preferences to choose cluster node.
 *
 * Returns:
 *       A newly allocated mongoc_cursor_t that should be freed with
 *       mongoc_cursor_destroy().
 *
 *       The client used by mongoc_collection_t must be valid for the
 *       lifetime of the resulting mongoc_cursor_t.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

mongoc_cursor_t *
mongoc_collection_find (mongoc_collection_t *collection,       /* IN */
                        mongoc_query_flags_t flags,            /* IN */
                        uint32_t skip,                         /* IN */
                        uint32_t limit,                        /* IN */
                        uint32_t batch_size,                   /* IN */
                        const bson_t *query,                   /* IN */
                        const bson_t *fields,                  /* IN */
                        const mongoc_read_prefs_t *read_prefs) /* IN */
{
   bool has_unwrapped;
   bson_t unwrapped;
   bson_error_t error = {0};
   bson_t opts;
   bool slave_ok;
   mongoc_cursor_t *cursor;
   BSON_ASSERT (collection);
   BSON_ASSERT (query);

   bson_clear (&collection->gle);

   bson_init (&opts);
   _mongoc_cursor_flags_to_opts (flags, &opts, &slave_ok);
   /* check if the query is wrapped in $query */
   has_unwrapped = _mongoc_cursor_translate_dollar_query_opts (
      query, &opts, &unwrapped, &error);
   if (!bson_empty0 (fields)) {
      bson_append_document (
         &opts, MONGOC_CURSOR_PROJECTION, MONGOC_CURSOR_PROJECTION_LEN, fields);
   }
   cursor = _mongoc_cursor_find_new (collection->client,
                                     collection->ns,
                                     has_unwrapped ? &unwrapped : query,
                                     &opts,
                                     read_prefs,
                                     collection->read_prefs,
                                     collection->read_concern);
   if (skip) {
      _mongoc_cursor_set_opt_int64 (cursor, MONGOC_CURSOR_SKIP, skip);
   }
   if (limit) {
      /* limit must be cast to int32_t. Although the argument is a uint32_t,
       * callers can specify a negative limit by casting to a signed int32_t
       * value to uint32_t. E.g. to set a limit of -4, the caller passes
       * UINT32_MAX - 3 */
      (void) mongoc_cursor_set_limit (cursor, (int32_t) limit);
   }
   if (batch_size) {
      mongoc_cursor_set_batch_size (cursor, batch_size);
   }
   bson_destroy (&unwrapped);
   bson_destroy (&opts);

   if (error.domain) {
      memcpy (&cursor->error, &error, sizeof (error));
   }

   return cursor;
}


/*
 *--------------------------------------------------------------------------
 *
 * mongoc_collection_find_with_opts --
 *
 *       Create a cursor with a query filter. All other options are
 *       specified in a free-form BSON document.
 *
 * Parameters:
 *       @collection: A mongoc_collection_t.
 *       @filter: The query to locate matching documents.
 *       @opts: Other options.
 *       @read_prefs: Optional read preferences to choose cluster node.
 *
 * Returns:
 *       A newly allocated mongoc_cursor_t that should be freed with
 *       mongoc_cursor_destroy().
 *
 *       The client used by mongoc_collection_t must be valid for the
 *       lifetime of the resulting mongoc_cursor_t.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

mongoc_cursor_t *
mongoc_collection_find_with_opts (mongoc_collection_t *collection,
                                  const bson_t *filter,
                                  const bson_t *opts,
                                  const mongoc_read_prefs_t *read_prefs)
{
   BSON_ASSERT (collection);
   BSON_ASSERT (filter);

   bson_clear (&collection->gle);

   return _mongoc_cursor_find_new (collection->client,
                                   collection->ns,
                                   filter,
                                   opts,
                                   read_prefs,
                                   collection->read_prefs,
                                   collection->read_concern);
}


/*
 *--------------------------------------------------------------------------
 *
 * mongoc_collection_command --
 *
 *       Executes a command on a cluster node matching @read_prefs. If
 *       @read_prefs is not provided, it will be run on the primary node.
 *
 *       This function will always return a mongoc_cursor_t.
 *
 * Parameters:
 *       @collection: A mongoc_collection_t.
 *       @flags: Bitwise-or'd flags for command.
 *       @skip: Number of documents to skip, typically 0.
 *       @limit : Number of documents to return
 *       @batch_size : Batch size
 *       @query: The command to execute.
 *       @fields: The fields to return, or NULL.
 *       @read_prefs: Command read preferences or NULL.
 *
 * Returns:
 *       None.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

mongoc_cursor_t *
mongoc_collection_command (mongoc_collection_t *collection,
                           mongoc_query_flags_t flags,
                           uint32_t skip,
                           uint32_t limit,
                           uint32_t batch_size,
                           const bson_t *query,
                           const bson_t *fields,
                           const mongoc_read_prefs_t *read_prefs)
{
   char ns[MONGOC_NAMESPACE_MAX];
   mongoc_cursor_t *cursor;

   BSON_ASSERT (collection);
   BSON_ASSERT (query);

   if (!read_prefs) {
      read_prefs = collection->read_prefs;
   }

   bson_clear (&collection->gle);

   if (NULL == strstr (collection->collection, "$cmd")) {
      bson_snprintf (ns, sizeof ns, "%s.$cmd", collection->db);
   } else {
      bson_snprintf (ns, sizeof ns, "%s", collection->db);
   }

   /* Server Selection Spec: "The generic command method has a default read
    * preference of mode 'primary'. The generic command method MUST ignore any
    * default read preference from client, database or collection
    * configuration. The generic command method SHOULD allow an optional read
    * preference argument."
    */

   /* flags, skip, limit, batch_size, fields are unused */
   cursor = _mongoc_cursor_cmd_deprecated_new (
      collection->client, ns, query, read_prefs);
   return cursor;
}


bool
mongoc_collection_read_command_with_opts (mongoc_collection_t *collection,
                                          const bson_t *command,
                                          const mongoc_read_prefs_t *read_prefs,
                                          const bson_t *opts,
                                          bson_t *reply,
                                          bson_error_t *error)
{
   BSON_ASSERT (collection);

   return _mongoc_client_command_with_opts (collection->client,
                                            collection->db,
                                            command,
                                            MONGOC_CMD_READ,
                                            opts,
                                            MONGOC_QUERY_NONE,
                                            read_prefs,
                                            collection->read_prefs,
                                            collection->read_concern,
                                            collection->write_concern,
                                            reply,
                                            error);
}


bool
mongoc_collection_write_command_with_opts (mongoc_collection_t *collection,
                                           const bson_t *command,
                                           const bson_t *opts,
                                           bson_t *reply,
                                           bson_error_t *error)
{
   BSON_ASSERT (collection);

   return _mongoc_client_command_with_opts (collection->client,
                                            collection->db,
                                            command,
                                            MONGOC_CMD_WRITE,
                                            opts,
                                            MONGOC_QUERY_NONE,
                                            NULL,
                                            collection->read_prefs,
                                            collection->read_concern,
                                            collection->write_concern,
                                            reply,
                                            error);
}


bool
mongoc_collection_read_write_command_with_opts (
   mongoc_collection_t *collection,
   const bson_t *command,
   const mongoc_read_prefs_t *read_prefs /* IGNORED */,
   const bson_t *opts,
   bson_t *reply,
   bson_error_t *error)
{
   BSON_ASSERT (collection);

   return _mongoc_client_command_with_opts (collection->client,
                                            collection->db,
                                            command,
                                            MONGOC_CMD_RW,
                                            opts,
                                            MONGOC_QUERY_NONE,
                                            read_prefs,
                                            collection->read_prefs,
                                            collection->read_concern,
                                            collection->write_concern,
                                            reply,
                                            error);
}


bool
mongoc_collection_command_with_opts (mongoc_collection_t *collection,
                                     const bson_t *command,
                                     const mongoc_read_prefs_t *read_prefs,
                                     const bson_t *opts,
                                     bson_t *reply,
                                     bson_error_t *error)
{
   BSON_ASSERT (collection);

   /* Server Selection Spec: "The generic command method has a default read
    * preference of mode 'primary'. The generic command method MUST ignore any
    * default read preference from client, database or collection
    * configuration. The generic command method SHOULD allow an optional read
    * preference argument." */

   return _mongoc_client_command_with_opts (collection->client,
                                            collection->db,
                                            command,
                                            MONGOC_CMD_RAW,
                                            opts,
                                            MONGOC_QUERY_NONE,
                                            read_prefs,
                                            NULL /* default prefs */,
                                            collection->read_concern,
                                            collection->write_concern,
                                            reply,
                                            error);
}


bool
mongoc_collection_command_simple (mongoc_collection_t *collection,
                                  const bson_t *command,
                                  const mongoc_read_prefs_t *read_prefs,
                                  bson_t *reply,
                                  bson_error_t *error)
{
   BSON_ASSERT (collection);
   BSON_ASSERT (command);

   bson_clear (&collection->gle);

   /* Server Selection Spec: "The generic command method has a default read
    * preference of mode 'primary'. The generic command method MUST ignore any
    * default read preference from client, database or collection
    * configuration. The generic command method SHOULD allow an optional read
    * preference argument."
    */

   return _mongoc_client_command_with_opts (collection->client,
                                            collection->db,
                                            command,
                                            MONGOC_CMD_READ,
                                            NULL /* opts */,
                                            MONGOC_QUERY_NONE,
                                            read_prefs,
                                            NULL /* default prefs */,
                                            NULL /* read concern */,
                                            NULL /* write concern */,
                                            reply,
                                            error);
}

/*
 *--------------------------------------------------------------------------
 *
 * mongoc_collection_count --
 *
 *       Count the number of documents matching @query.
 *
 * Parameters:
 *       @flags: A mongoc_query_flags_t describing the query flags or 0.
 *       @query: The query to perform or NULL for {}.
 *       @skip: The $skip to perform within the query or 0.
 *       @limit: The $limit to perform within the query or 0.
 *       @read_prefs: desired read preferences or NULL.
 *       @error: A location for an error or NULL.
 *
 * Returns:
 *       -1 on failure; otherwise the number of matching documents.
 *
 * Side effects:
 *       @error is set upon failure if non-NULL.
 *
 *--------------------------------------------------------------------------
 */

int64_t
mongoc_collection_count (mongoc_collection_t *collection,       /* IN */
                         mongoc_query_flags_t flags,            /* IN */
                         const bson_t *query,                   /* IN */
                         int64_t skip,                          /* IN */
                         int64_t limit,                         /* IN */
                         const mongoc_read_prefs_t *read_prefs, /* IN */
                         bson_error_t *error)                   /* OUT */
{
   int64_t ret;
   bson_t opts = BSON_INITIALIZER;

   /* Complex types must be parts of `opts`, otherwise we can't
    * follow various specs that require validation etc */
   if (collection->read_concern->level != NULL) {
      const bson_t *read_concern_bson;

      read_concern_bson =
         _mongoc_read_concern_get_bson (collection->read_concern);
      BSON_APPEND_DOCUMENT (&opts, "readConcern", read_concern_bson);
   }

   /* Server Selection Spec: "may-use-secondary" commands SHOULD take a read
    * preference argument and otherwise MUST use the default read preference
    * from client, database or collection configuration. */
   BEGIN_IGNORE_DEPRECATIONS
   ret = mongoc_collection_count_with_opts (
      collection, flags, query, skip, limit, &opts, read_prefs, error);
   END_IGNORE_DEPRECATIONS

   bson_destroy (&opts);
   return ret;
}


int64_t
mongoc_collection_count_with_opts (
   mongoc_collection_t *collection,       /* IN */
   mongoc_query_flags_t flags,            /* IN */
   const bson_t *query,                   /* IN */
   int64_t skip,                          /* IN */
   int64_t limit,                         /* IN */
   const bson_t *opts,                    /* IN */
   const mongoc_read_prefs_t *read_prefs, /* IN */
   bson_error_t *error)                   /* OUT */
{
   bson_iter_t iter;
   int64_t ret = -1;
   bool success;
   bson_t reply;
   bson_t cmd = BSON_INITIALIZER;
   bson_t q;

   ENTRY;

   BSON_ASSERT (collection);

   bson_append_utf8 (
      &cmd, "count", 5, collection->collection, collection->collectionlen);
   if (query) {
      bson_append_document (&cmd, "query", 5, query);
   } else {
      bson_init (&q);
      bson_append_document (&cmd, "query", 5, &q);
      bson_destroy (&q);
   }
   if (limit) {
      bson_append_int64 (&cmd, "limit", 5, limit);
   }
   if (skip) {
      bson_append_int64 (&cmd, "skip", 4, skip);
   }

   success = _mongoc_client_command_with_opts (collection->client,
                                               collection->db,
                                               &cmd,
                                               MONGOC_CMD_READ,
                                               opts,
                                               flags,
                                               read_prefs,
                                               collection->read_prefs,
                                               collection->read_concern,
                                               collection->write_concern,
                                               &reply,
                                               error);

   if (success) {
      if (bson_iter_init_find (&iter, &reply, "n")) {
         ret = bson_iter_as_int64 (&iter);
      }
   }

   bson_destroy (&reply);
   bson_destroy (&cmd);

   RETURN (ret);
}


int64_t
mongoc_collection_estimated_document_count (
   mongoc_collection_t *coll,
   const bson_t *opts,
   const mongoc_read_prefs_t *read_prefs,
   bson_t *reply,
   bson_error_t *error)
{
   bson_iter_t iter;
   int64_t count = -1;
   bool ret;
   bson_t reply_local;
   bson_t *reply_ptr;
   bson_t cmd = BSON_INITIALIZER;

   ENTRY;

   BSON_ASSERT (coll);

   reply_ptr = reply ? reply : &reply_local;
   bson_append_utf8 (&cmd, "count", 5, coll->collection, coll->collectionlen);

   ret = _mongoc_client_command_with_opts (coll->client,
                                           coll->db,
                                           &cmd,
                                           MONGOC_CMD_READ,
                                           opts,
                                           MONGOC_QUERY_NONE,
                                           read_prefs,
                                           coll->read_prefs,
                                           coll->read_concern,
                                           coll->write_concern,
                                           reply_ptr,
                                           error);

   if (ret) {
      if (bson_iter_init_find (&iter, reply_ptr, "n")) {
         count = bson_iter_as_int64 (&iter);
      }
   }

   if (!reply) {
      bson_destroy (&reply_local);
   }
   bson_destroy (&cmd);

   RETURN (count);
}


/* --------------------------------------------------------------------------
 *
 * _make_aggregate_for_count --
 *
 *       Construct an aggregate pipeline with the following form:
 *       { pipeline: [
 *           { $match: {...} },
 *           { $group: { _id: null, n: { sum: 1 } } },
 *           { $skip: ... },
 *           { $limit: ... }
 *         ]
 *       }
 *
 *--------------------------------------------------------------------------
 */
static void
_make_aggregate_for_count (const mongoc_collection_t *coll,
                           const bson_t *filter,
                           const bson_t *opts,
                           bson_t *out)
{
   bson_iter_t iter;
   bson_t pipeline;
   bson_t match_stage;
   bson_t group_stage;
   bson_t group_stage_doc;
   bson_t sum;
   bson_t empty;
   const char *keys[] = {"0", "1", "2", "3"};
   int key = 0;

   bson_init (out);
   bson_append_utf8 (
      out, "aggregate", 9, coll->collection, coll->collectionlen);
   bson_append_document_begin (out, "cursor", 6, &empty);
   bson_append_document_end (out, &empty);
   bson_append_array_begin (out, "pipeline", 8, &pipeline);
   if (!bson_empty (filter)) {
      bson_append_document_begin (&pipeline, keys[key++], 1, &match_stage);
      bson_append_document (&match_stage, "$match", 6, filter);
      bson_append_document_end (&pipeline, &match_stage);
   }
   /* if @opts includes "skip", or "count", append $skip and $count stages to
    * the aggregate pipeline. */
   if (opts && bson_iter_init_find (&iter, opts, "skip")) {
      bson_t skip_stage;
      bson_append_document_begin (&pipeline, keys[key++], 1, &skip_stage);
      bson_append_value (&skip_stage, "$skip", 5, bson_iter_value (&iter));
      bson_append_document_end (&pipeline, &skip_stage);
   }
   if (opts && bson_iter_init_find (&iter, opts, "limit")) {
      bson_t limit_stage;
      bson_append_document_begin (&pipeline, keys[key++], 1, &limit_stage);
      bson_append_value (&limit_stage, "$limit", 6, bson_iter_value (&iter));
      bson_append_document_end (&pipeline, &limit_stage);
   }
   bson_append_document_begin (&pipeline, keys[key], 1, &group_stage);
   bson_append_document_begin (&group_stage, "$group", 6, &group_stage_doc);
   bson_append_null (&group_stage_doc, "_id", 3);
   bson_append_document_begin (&group_stage_doc, "n", 1, &sum);
   bson_append_int32 (&sum, "$sum", 4, 1);
   bson_append_document_end (&group_stage_doc, &sum);
   bson_append_document_end (&group_stage, &group_stage_doc);
   bson_append_document_end (&pipeline, &group_stage);
   bson_append_array_end (out, &pipeline);
}


int64_t
mongoc_collection_count_documents (mongoc_collection_t *coll,
                                   const bson_t *filter,
                                   const bson_t *opts,
                                   const mongoc_read_prefs_t *read_prefs,
                                   bson_t *reply,
                                   bson_error_t *error)
{
   bson_t aggregate_cmd;
   bson_t aggregate_opts;
   bool ret;
   const bson_t *result;
   mongoc_cursor_t *cursor = NULL;
   int64_t count = -1;
   bson_t cmd_reply;
   bson_iter_t iter;

   ENTRY;

   BSON_ASSERT (coll);
   BSON_ASSERT (filter);

   _make_aggregate_for_count (coll, filter, opts, &aggregate_cmd);
   bson_init (&aggregate_opts);
   if (opts) {
      bson_copy_to_excluding_noinit (
         opts, &aggregate_opts, "skip", "limit", NULL);
   }

   ret = mongoc_collection_read_command_with_opts (
      coll, &aggregate_cmd, read_prefs, &aggregate_opts, &cmd_reply, error);
   bson_destroy (&aggregate_cmd);
   bson_destroy (&aggregate_opts);
   if (reply) {
      bson_copy_to (&cmd_reply, reply);
   }

   if (!ret) {
      bson_destroy (&cmd_reply);
      GOTO (done);
   }

   /* steals reply */
   cursor = mongoc_cursor_new_from_command_reply_with_opts (
      coll->client, &cmd_reply, NULL);
   BSON_ASSERT (mongoc_cursor_get_id (cursor) == 0);
   ret = mongoc_cursor_next (cursor, &result);
   if (!ret) {
      if (mongoc_cursor_error (cursor, error)) {
         GOTO (done);
      } else {
         count = 0;
         GOTO (done);
      }
   }

   if (bson_iter_init_find (&iter, result, "n") &&
       BSON_ITER_HOLDS_INT (&iter)) {
      count = bson_iter_as_int64 (&iter);
   }

done:
   if (cursor) {
      mongoc_cursor_destroy (cursor);
   }
   RETURN (count);
}


/*
 *--------------------------------------------------------------------------
 *
 * mongoc_collection_drop --
 *
 *       Request the MongoDB server drop the collection.
 *
 * Returns:
 *       true if successful; otherwise false and @error is set.
 *
 * Side effects:
 *       @error is set upon failure.
 *
 *--------------------------------------------------------------------------
 */

bool
mongoc_collection_drop (mongoc_collection_t *collection, /* IN */
                        bson_error_t *error)             /* OUT */
{
   return mongoc_collection_drop_with_opts (collection, NULL, error);
}


bool
mongoc_collection_drop_with_opts (mongoc_collection_t *collection,
                                  const bson_t *opts,
                                  bson_error_t *error)
{
   bool ret;
   bson_t cmd;

   BSON_ASSERT (collection);

   bson_init (&cmd);
   bson_append_utf8 (
      &cmd, "drop", 4, collection->collection, collection->collectionlen);

   ret = _mongoc_client_command_with_opts (collection->client,
                                           collection->db,
                                           &cmd,
                                           MONGOC_CMD_WRITE,
                                           opts,
                                           MONGOC_QUERY_NONE,
                                           NULL, /* user prefs */
                                           collection->read_prefs,
                                           collection->read_concern,
                                           collection->write_concern,
                                           NULL, /* reply */
                                           error);
   bson_destroy (&cmd);

   return ret;
}


/*
 *--------------------------------------------------------------------------
 *
 * mongoc_collection_drop_index --
 *
 *       Request the MongoDB server drop the named index.
 *
 * Returns:
 *       true if successful; otherwise false and @error is set.
 *
 * Side effects:
 *       @error is setup upon failure if non-NULL.
 *
 *--------------------------------------------------------------------------
 */

bool
mongoc_collection_drop_index (mongoc_collection_t *collection, /* IN */
                              const char *index_name,          /* IN */
                              bson_error_t *error)             /* OUT */
{
   return mongoc_collection_drop_index_with_opts (
      collection, index_name, NULL, error);
}


bool
mongoc_collection_drop_index_with_opts (mongoc_collection_t *collection,
                                        const char *index_name,
                                        const bson_t *opts,
                                        bson_error_t *error)
{
   bool ret;
   bson_t cmd;

   BSON_ASSERT (collection);
   BSON_ASSERT (index_name);

   bson_init (&cmd);
   bson_append_utf8 (&cmd,
                     "dropIndexes",
                     -1,
                     collection->collection,
                     collection->collectionlen);
   bson_append_utf8 (&cmd, "index", -1, index_name, -1);

   ret = _mongoc_client_command_with_opts (collection->client,
                                           collection->db,
                                           &cmd,
                                           MONGOC_CMD_WRITE,
                                           opts,
                                           MONGOC_QUERY_NONE,
                                           NULL, /* user prefs */
                                           collection->read_prefs,
                                           collection->read_concern,
                                           collection->write_concern,
                                           NULL, /* reply */
                                           error);
   bson_destroy (&cmd);

   return ret;
}


char *
mongoc_collection_keys_to_index_string (const bson_t *keys)
{
   bson_string_t *s;
   bson_iter_t iter;
   int i = 0;

   BSON_ASSERT (keys);

   if (!bson_iter_init (&iter, keys)) {
      return NULL;
   }

   s = bson_string_new (NULL);

   while (bson_iter_next (&iter)) {
      /* Index type can be specified as a string ("2d") or as an integer
       * representing direction */
      if (bson_iter_type (&iter) == BSON_TYPE_UTF8) {
         bson_string_append_printf (s,
                                    (i++ ? "_%s_%s" : "%s_%s"),
                                    bson_iter_key (&iter),
                                    bson_iter_utf8 (&iter, NULL));
      } else {
         bson_string_append_printf (s,
                                    (i++ ? "_%s_%d" : "%s_%d"),
                                    bson_iter_key (&iter),
                                    bson_iter_int32 (&iter));
      }
   }

   return bson_string_free (s, false);
}


bool
mongoc_collection_create_index (mongoc_collection_t *collection,
                                const bson_t *keys,
                                const mongoc_index_opt_t *opt,
                                bson_error_t *error)
{
   bson_t reply;
   bool ret;

   BEGIN_IGNORE_DEPRECATIONS

   ret = mongoc_collection_create_index_with_opts (
      collection, keys, opt, NULL, &reply, error);

   END_IGNORE_DEPRECATIONS

   bson_destroy (&reply);
   return ret;
}

bool
mongoc_collection_create_index_with_opts (mongoc_collection_t *collection,
                                          const bson_t *keys,
                                          const mongoc_index_opt_t *opt,
                                          const bson_t *opts,
                                          bson_t *reply,
                                          bson_error_t *error)
{
   mongoc_create_index_opts_t parsed;
   mongoc_cmd_parts_t parts;
   const mongoc_index_opt_t *def_opt;
   const mongoc_index_opt_geo_t *def_geo;
   const char *name;
   bson_t cmd = BSON_INITIALIZER;
   bson_t ar;
   bson_t doc;
   bson_t storage_doc;
   bson_t wt_doc;
   const mongoc_index_opt_geo_t *geo_opt;
   const mongoc_index_opt_storage_t *storage_opt;
   const mongoc_index_opt_wt_t *wt_opt;
   char *alloc_name = NULL;
   bool ret = false;
   bool reply_initialized = false;
   bool has_collation = false;
   mongoc_server_stream_t *server_stream = NULL;
   mongoc_cluster_t *cluster;

   ENTRY;

   BSON_ASSERT (collection);
   BSON_ASSERT (keys);

   def_opt = mongoc_index_opt_get_default ();
   opt = opt ? opt : def_opt;

   mongoc_cmd_parts_init (
      &parts, collection->client, collection->db, MONGOC_QUERY_NONE, &cmd);
   parts.is_write_command = true;

   if (!_mongoc_create_index_opts_parse (
          collection->client, opts, &parsed, error)) {
      GOTO (done);
   }

   /*
    * Generate the key name if it was not provided.
    */
   name = (opt->name != def_opt->name) ? opt->name : NULL;
   if (!name) {
      alloc_name = mongoc_collection_keys_to_index_string (keys);
      if (alloc_name) {
         name = alloc_name;
      } else {
         bson_set_error (
            error,
            MONGOC_ERROR_BSON,
            MONGOC_ERROR_BSON_INVALID,
            "Cannot generate index name from invalid `keys` argument");
         GOTO (done);
      }
   }

   /*
    * Build our createIndexes command to send to the server.
    */
   BSON_ASSERT (
      BSON_APPEND_UTF8 (&cmd, "createIndexes", collection->collection));
   bson_append_array_begin (&cmd, "indexes", 7, &ar);
   bson_append_document_begin (&ar, "0", 1, &doc);
   BSON_ASSERT (BSON_APPEND_DOCUMENT (&doc, "key", keys));
   BSON_ASSERT (BSON_APPEND_UTF8 (&doc, "name", name));
   if (opt->background) {
      BSON_ASSERT (BSON_APPEND_BOOL (&doc, "background", true));
   }
   if (opt->unique) {
      BSON_ASSERT (BSON_APPEND_BOOL (&doc, "unique", true));
   }
   if (opt->drop_dups) {
      BSON_ASSERT (BSON_APPEND_BOOL (&doc, "dropDups", true));
   }
   if (opt->sparse) {
      BSON_ASSERT (BSON_APPEND_BOOL (&doc, "sparse", true));
   }
   if (opt->expire_after_seconds != def_opt->expire_after_seconds) {
      BSON_ASSERT (BSON_APPEND_INT32 (
         &doc, "expireAfterSeconds", opt->expire_after_seconds));
   }
   if (opt->v != def_opt->v) {
      BSON_ASSERT (BSON_APPEND_INT32 (&doc, "v", opt->v));
   }
   if (opt->weights && (opt->weights != def_opt->weights)) {
      BSON_ASSERT (BSON_APPEND_DOCUMENT (&doc, "weights", opt->weights));
   }
   if (opt->default_language != def_opt->default_language) {
      BSON_ASSERT (
         BSON_APPEND_UTF8 (&doc, "default_language", opt->default_language));
   }
   if (opt->language_override != def_opt->language_override) {
      BSON_ASSERT (
         BSON_APPEND_UTF8 (&doc, "language_override", opt->language_override));
   }
   if (opt->partial_filter_expression) {
      BSON_ASSERT (BSON_APPEND_DOCUMENT (
         &doc, "partialFilterExpression", opt->partial_filter_expression));
   }
   if (opt->collation) {
      BSON_ASSERT (BSON_APPEND_DOCUMENT (&doc, "collation", opt->collation));
      has_collation = true;
   }
   if (opt->geo_options) {
      geo_opt = opt->geo_options;
      def_geo = mongoc_index_opt_geo_get_default ();
      if (geo_opt->twod_sphere_version != def_geo->twod_sphere_version) {
         BSON_ASSERT (BSON_APPEND_INT32 (
            &doc, "2dsphereIndexVersion", geo_opt->twod_sphere_version));
      }
      if (geo_opt->twod_bits_precision != def_geo->twod_bits_precision) {
         BSON_ASSERT (
            BSON_APPEND_INT32 (&doc, "bits", geo_opt->twod_bits_precision));
      }
      if (geo_opt->twod_location_min != def_geo->twod_location_min) {
         BSON_ASSERT (
            BSON_APPEND_DOUBLE (&doc, "min", geo_opt->twod_location_min));
      }
      if (geo_opt->twod_location_max != def_geo->twod_location_max) {
         BSON_ASSERT (
            BSON_APPEND_DOUBLE (&doc, "max", geo_opt->twod_location_max));
      }
      if (geo_opt->haystack_bucket_size != def_geo->haystack_bucket_size) {
         BSON_ASSERT (BSON_APPEND_DOUBLE (
            &doc, "bucketSize", geo_opt->haystack_bucket_size));
      }
   }

   if (opt->storage_options) {
      storage_opt = opt->storage_options;
      switch (storage_opt->type) {
      case MONGOC_INDEX_STORAGE_OPT_WIREDTIGER:
         wt_opt = (mongoc_index_opt_wt_t *) storage_opt;
         BSON_APPEND_DOCUMENT_BEGIN (&doc, "storageEngine", &storage_doc);
         BSON_APPEND_DOCUMENT_BEGIN (&storage_doc, "wiredTiger", &wt_doc);
         BSON_ASSERT (
            BSON_APPEND_UTF8 (&wt_doc, "configString", wt_opt->config_str));
         bson_append_document_end (&storage_doc, &wt_doc);
         bson_append_document_end (&doc, &storage_doc);
         break;
      default:
         break;
      }
   }

   bson_append_document_end (&ar, &doc);
   bson_append_array_end (&cmd, &ar);

   server_stream = mongoc_cluster_stream_for_writes (
      &collection->client->cluster, parsed.client_session, reply, error);

   if (!server_stream) {
      reply_initialized = true;
      GOTO (done);
   }

   if (!mongoc_cmd_parts_set_write_concern (&parts,
                                            parsed.writeConcern,
                                            server_stream->sd->max_wire_version,
                                            error)) {
      GOTO (done);
   }

   if (has_collation &&
       server_stream->sd->max_wire_version < WIRE_VERSION_COLLATION) {
      bson_set_error (error,
                      MONGOC_ERROR_COMMAND,
                      MONGOC_ERROR_PROTOCOL_BAD_WIRE_VERSION,
                      "The selected server does not support collation");
      GOTO (done);
   }

   parts.assembled.session = parsed.client_session;
   if (!bson_concat (&parts.extra, &parsed.extra)) {
      bson_set_error (error,
                      MONGOC_ERROR_COMMAND,
                      MONGOC_ERROR_COMMAND_INVALID_ARG,
                      "'opts' is too large");
      GOTO (done);
   }

   cluster = &collection->client->cluster;
   if (mongoc_cmd_parts_assemble (&parts, server_stream, error)) {
      ret = mongoc_cluster_run_command_monitored (
         cluster, &parts.assembled, reply, error);
   } else {
      _mongoc_bson_init_if_set (reply);
   }

   reply_initialized = true;

   if (ret) {
      if (reply) {
         ret = !_mongoc_parse_wc_err (reply, error);
      }
   }

done:
   bson_destroy (&cmd);
   bson_free (alloc_name);
   _mongoc_create_index_opts_cleanup (&parsed);
   mongoc_server_stream_cleanup (server_stream);
   mongoc_cmd_parts_cleanup (&parts);
   if (!reply_initialized && reply) {
      bson_init (reply);
   }

   RETURN (ret);
}


bool
mongoc_collection_ensure_index (mongoc_collection_t *collection,
                                const bson_t *keys,
                                const mongoc_index_opt_t *opt,
                                bson_error_t *error)
{
   BEGIN_IGNORE_DEPRECATIONS
   return mongoc_collection_create_index (collection, keys, opt, error);
   END_IGNORE_DEPRECATIONS
}


mongoc_cursor_t *
mongoc_collection_find_indexes (mongoc_collection_t *collection,
                                bson_error_t *error)
{
   mongoc_cursor_t *cursor;

   cursor = mongoc_collection_find_indexes_with_opts (collection, NULL);

   (void) mongoc_cursor_error (cursor, error);

   return cursor;
}


mongoc_cursor_t *
mongoc_collection_find_indexes_with_opts (mongoc_collection_t *collection,
                                          const bson_t *opts)
{
   mongoc_cursor_t *cursor;
   bson_t cmd = BSON_INITIALIZER;
   bson_t child;
   bson_error_t error;

   BSON_ASSERT (collection);

   bson_append_utf8 (&cmd,
                     "listIndexes",
                     -1,
                     collection->collection,
                     collection->collectionlen);

   BSON_APPEND_DOCUMENT_BEGIN (&cmd, "cursor", &child);
   bson_append_document_end (&cmd, &child);

   /* No read preference. Index Enumeration Spec: "run listIndexes on the
    * primary node in replicaSet mode". */
   cursor = _mongoc_cursor_cmd_new (collection->client,
                                    collection->ns,
                                    &cmd,
                                    opts,
                                    NULL /* read prefs */,
                                    NULL /* read concern */);

   if (!mongoc_cursor_error (cursor, &error)) {
      _mongoc_cursor_prime (cursor);
   }

   if (mongoc_cursor_error (cursor, &error) &&
       error.code == MONGOC_ERROR_COLLECTION_DOES_NOT_EXIST) {
      /* collection does not exist. from spec: return no documents but no err:
       * https://github.com/mongodb/specifications/blob/master/source/enumerate-indexes.rst#enumeration-getting-index-information
       */
      _mongoc_cursor_set_empty (cursor);
   }

   bson_destroy (&cmd);

   return cursor;
}

/*
 *--------------------------------------------------------------------------
 *
 * mongoc_collection_insert_bulk --
 *
 *       Bulk insert documents into a MongoDB collection.
 *
 * Parameters:
 *       @collection: A mongoc_collection_t.
 *       @flags: flags for the insert or 0.
 *       @documents: The documents to insert.
 *       @n_documents: The number of documents to insert.
 *       @write_concern: A write concern or NULL.
 *       @error: a location for an error or NULL.
 *
 * Returns:
 *       true if successful; otherwise false and @error is set.
 *
 *       If the write concern does not dictate checking the result of the
 *       insert, then true may be returned even though the document was
 *       not actually inserted on the MongoDB server or cluster.
 *
 * Side effects:
 *       @collection->gle is setup, depending on write_concern->w value.
 *       @error may be set upon failure if non-NULL.
 *
 *--------------------------------------------------------------------------
 */

bool
mongoc_collection_insert_bulk (mongoc_collection_t *collection,
                               mongoc_insert_flags_t flags,
                               const bson_t **documents,
                               uint32_t n_documents,
                               const mongoc_write_concern_t *write_concern,
                               bson_error_t *error)
{
   mongoc_write_command_t command;
   mongoc_write_result_t result;
   mongoc_bulk_write_flags_t write_flags = MONGOC_BULK_WRITE_FLAGS_INIT;
   uint32_t i;
   bool ret;

   BSON_ASSERT (collection);
   BSON_ASSERT (documents);

   if (!write_concern) {
      write_concern = collection->write_concern;
   }

   if (!(flags & MONGOC_INSERT_NO_VALIDATE)) {
      for (i = 0; i < n_documents; i++) {
         if (!_mongoc_validate_new_document (
                documents[i], _mongoc_default_insert_vflags, error)) {
            RETURN (false);
         }
      }
   }

   bson_clear (&collection->gle);

   _mongoc_write_result_init (&result);

   write_flags.ordered = !(flags & MONGOC_INSERT_CONTINUE_ON_ERROR);

   _mongoc_write_command_init_insert (
      &command,
      NULL,
      NULL,
      write_flags,
      ++collection->client->cluster.operation_id,
      true);

   for (i = 0; i < n_documents; i++) {
      _mongoc_write_command_insert_append (&command, documents[i]);
   }

   _mongoc_collection_write_command_execute (
      &command, collection, write_concern, NULL, &result);

   collection->gle = bson_new ();
   ret = MONGOC_WRITE_RESULT_COMPLETE (&result,
                                       collection->client->error_api_version,
                                       write_concern,
                                       /* no error domain override */
                                       (mongoc_error_domain_t) 0,
                                       collection->gle,
                                       error);

   _mongoc_write_result_destroy (&result);
   _mongoc_write_command_destroy (&command);

   return ret;
}


bool
mongoc_collection_insert (mongoc_collection_t *collection,
                          mongoc_insert_flags_t flags,
                          const bson_t *document,
                          const mongoc_write_concern_t *write_concern,
                          bson_error_t *error)
{
   bson_t opts = BSON_INITIALIZER;
   bson_t reply;
   bool r;

   bson_clear (&collection->gle);

   if (flags & MONGOC_INSERT_NO_VALIDATE) {
      bson_append_bool (&opts, "validate", 8, false);
   }

   if (write_concern) {
      mongoc_write_concern_append ((mongoc_write_concern_t *) write_concern,
                                   &opts);
   }

   r =
      mongoc_collection_insert_one (collection, document, &opts, &reply, error);

   collection->gle = bson_copy (&reply);
   bson_destroy (&reply);
   bson_destroy (&opts);

   return r;
}


/*
 *--------------------------------------------------------------------------
 *
 * mongoc_collection_insert_one --
 *
 *       Insert a document into a MongoDB collection.
 *
 * Parameters:
 *       @collection: A mongoc_collection_t.
 *       @document: The document to insert.
 *       @opts: Standard command options.
 *       @reply: Optional. Uninitialized doc to receive the update result.
 *       @error: A location for an error or NULL.
 *
 * Returns:
 *       true if successful; otherwise false and @error is set.
 *
 *       If the write concern does not dictate checking the result of the
 *       insert, then true may be returned even though the document was
 *       not actually inserted on the MongoDB server or cluster.
 *
 *--------------------------------------------------------------------------
 */

bool
mongoc_collection_insert_one (mongoc_collection_t *collection,
                              const bson_t *document,
                              const bson_t *opts,
                              bson_t *reply,
                              bson_error_t *error)
{
   mongoc_insert_one_opts_t insert_one_opts;
   mongoc_write_command_t command;
   mongoc_write_result_t result;
   bool ret = false;

   ENTRY;

   BSON_ASSERT (collection);
   BSON_ASSERT (document);

   _mongoc_bson_init_if_set (reply);

   if (!_mongoc_insert_one_opts_parse (
          collection->client, opts, &insert_one_opts, error)) {
      GOTO (done);
   }

   if (!_mongoc_validate_new_document (
          document, insert_one_opts.crud.validate, error)) {
      GOTO (done);
   }

   _mongoc_write_result_init (&result);
   _mongoc_write_command_init_insert_idl (
      &command,
      document,
      &insert_one_opts.extra,
      ++collection->client->cluster.operation_id,
      false);

   command.flags.bypass_document_validation = insert_one_opts.bypass;
   _mongoc_collection_write_command_execute_idl (
      &command, collection, &insert_one_opts.crud, &result);

   ret = MONGOC_WRITE_RESULT_COMPLETE (&result,
                                       collection->client->error_api_version,
                                       insert_one_opts.crud.writeConcern,
                                       /* no error domain override */
                                       (mongoc_error_domain_t) 0,
                                       reply,
                                       error,
                                       "insertedCount");

   _mongoc_write_result_destroy (&result);
   _mongoc_write_command_destroy (&command);

done:
   _mongoc_insert_one_opts_cleanup (&insert_one_opts);
   RETURN (ret);
}


/*
 *--------------------------------------------------------------------------
 *
 * mongoc_collection_insert_many --
 *
 *       Insert documents into a MongoDB collection. Replaces
 *       mongoc_collection_insert_bulk.
 *
 * Parameters:
 *       @collection: A mongoc_collection_t.
 *       @documents: The documents to insert.
 *       @n_documents: Length of @documents array.
 *       @opts: Standard command options.
 *       @reply: Optional. Uninitialized doc to receive the update result.
 *       @error: A location for an error or NULL.
 *
 * Returns:
 *       true if successful; otherwise false and @error is set.
 *
 *       If the write concern does not dictate checking the result of the
 *       insert, then true may be returned even though the document was
 *       not actually inserted on the MongoDB server or cluster.
 *
 *--------------------------------------------------------------------------
 */

bool
mongoc_collection_insert_many (mongoc_collection_t *collection,
                               const bson_t **documents,
                               size_t n_documents,
                               const bson_t *opts,
                               bson_t *reply,
                               bson_error_t *error)
{
   mongoc_insert_many_opts_t insert_many_opts;
   mongoc_write_command_t command;
   mongoc_write_result_t result;
   size_t i;
   bool ret;

   ENTRY;

   BSON_ASSERT (collection);
   BSON_ASSERT (documents);

   _mongoc_bson_init_if_set (reply);

   if (!_mongoc_insert_many_opts_parse (
          collection->client, opts, &insert_many_opts, error)) {
      _mongoc_insert_many_opts_cleanup (&insert_many_opts);
      return false;
   }

   _mongoc_write_result_init (&result);
   _mongoc_write_command_init_insert_idl (
      &command,
      NULL,
      &insert_many_opts.extra,
      ++collection->client->cluster.operation_id,
      false);

   command.flags.ordered = insert_many_opts.ordered;
   command.flags.bypass_document_validation = insert_many_opts.bypass;

   for (i = 0; i < n_documents; i++) {
      if (!_mongoc_validate_new_document (
             documents[i], insert_many_opts.crud.validate, error)) {
         ret = false;
         GOTO (done);
      }

      _mongoc_write_command_insert_append (&command, documents[i]);
   }

   _mongoc_collection_write_command_execute_idl (
      &command, collection, &insert_many_opts.crud, &result);

   ret = MONGOC_WRITE_RESULT_COMPLETE (&result,
                                       collection->client->error_api_version,
                                       insert_many_opts.crud.writeConcern,
                                       /* no error domain override */
                                       (mongoc_error_domain_t) 0,
                                       reply,
                                       error,
                                       "insertedCount");

done:
   _mongoc_write_result_destroy (&result);
   _mongoc_write_command_destroy (&command);
   _mongoc_insert_many_opts_cleanup (&insert_many_opts);

   RETURN (ret);
}


/*
 *--------------------------------------------------------------------------
 *
 * mongoc_collection_update --
 *
 *       Updates one or more documents matching @selector with @update.
 *
 * Parameters:
 *       @collection: A mongoc_collection_t.
 *       @flags: The flags for the update.
 *       @selector: A bson_t containing your selector.
 *       @update: A bson_t containing your update document.
 *       @write_concern: The write concern or NULL.
 *       @error: A location for an error or NULL.
 *
 * Returns:
 *       true if successful; otherwise false and @error is set.
 *
 * Side effects:
 *       @collection->gle is setup, depending on write_concern->w value.
 *       @error is setup upon failure.
 *
 *--------------------------------------------------------------------------
 */

bool
mongoc_collection_update (mongoc_collection_t *collection,
                          mongoc_update_flags_t uflags,
                          const bson_t *selector,
                          const bson_t *update,
                          const mongoc_write_concern_t *write_concern,
                          bson_error_t *error)
{
   mongoc_bulk_write_flags_t write_flags = MONGOC_BULK_WRITE_FLAGS_INIT;
   mongoc_write_command_t command;
   mongoc_write_result_t result;
   bson_iter_t iter;
   bool ret;
   int flags = uflags;
   bson_t opts;

   ENTRY;

   BSON_ASSERT (collection);
   BSON_ASSERT (selector);
   BSON_ASSERT (update);

   bson_clear (&collection->gle);

   if (!write_concern) {
      write_concern = collection->write_concern;
   }

   if (!((uint32_t) flags & MONGOC_UPDATE_NO_VALIDATE) &&
       bson_iter_init (&iter, update) && bson_iter_next (&iter)) {
      if (bson_iter_key (&iter)[0] == '$') {
         /* update document, all keys must be $-operators */
         if (!_mongoc_validate_update (
                update, _mongoc_default_update_vflags, error)) {
            return false;
         }
      } else {
         if (!_mongoc_validate_replace (
                update, _mongoc_default_replace_vflags, error)) {
            return false;
         }
      }
   }

   bson_init (&opts);
   BSON_APPEND_BOOL (&opts, "upsert", !!(flags & MONGOC_UPDATE_UPSERT));
   BSON_APPEND_BOOL (&opts, "multi", !!(flags & MONGOC_UPDATE_MULTI_UPDATE));

   _mongoc_write_result_init (&result);
   _mongoc_write_command_init_update (
      &command,
      selector,
      update,
      &opts,
      write_flags,
      ++collection->client->cluster.operation_id);
   bson_destroy (&opts);

   _mongoc_collection_write_command_execute (
      &command, collection, write_concern, NULL, &result);

   collection->gle = bson_new ();
   ret = MONGOC_WRITE_RESULT_COMPLETE (&result,
                                       collection->client->error_api_version,
                                       write_concern,
                                       /* no error domain override */
                                       (mongoc_error_domain_t) 0,
                                       collection->gle,
                                       error);

   _mongoc_write_result_destroy (&result);
   _mongoc_write_command_destroy (&command);

   RETURN (ret);
}

static bool
_mongoc_collection_update_or_replace (
   mongoc_collection_t *collection,
   const bson_t *selector,
   const bson_t *update,
   mongoc_update_opts_t *update_opts,
   mongoc_write_bypass_document_validation_t bypass,
   const bson_t *array_filters,
   bson_t *extra,
   bson_t *reply,
   bson_error_t *error)
{
   mongoc_write_command_t command;
   mongoc_write_result_t result;
   mongoc_server_stream_t *server_stream = NULL;
   bool reply_initialized = false;
   bool ret = false;

   ENTRY;

   BSON_ASSERT (collection);
   BSON_ASSERT (selector);
   BSON_ASSERT (update);

   if (update_opts->upsert) {
      bson_append_bool (extra, "upsert", 6, true);
   }

   if (!bson_empty (&update_opts->collation)) {
      bson_append_document (extra, "collation", 9, &update_opts->collation);
   }

   if (!bson_empty0 (array_filters)) {
      bson_append_array (extra, "arrayFilters", 12, array_filters);
   }

   _mongoc_write_result_init (&result);
   _mongoc_write_command_init_update_idl (
      &command,
      selector,
      update,
      extra,
      ++collection->client->cluster.operation_id);

   command.flags.bypass_document_validation = bypass;
   if (!bson_empty (&update_opts->collation)) {
      command.flags.has_collation = true;
   }

   server_stream =
      mongoc_cluster_stream_for_writes (&collection->client->cluster,
                                        update_opts->crud.client_session,
                                        reply,
                                        error);

   if (!server_stream) {
      /* mongoc_cluster_stream_for_writes inits reply on error */
      reply_initialized = true;
      GOTO (done);
   }

   if (!bson_empty0 (array_filters)) {
      if (server_stream->sd->max_wire_version < WIRE_VERSION_ARRAY_FILTERS) {
         bson_set_error (error,
                         MONGOC_ERROR_COMMAND,
                         MONGOC_ERROR_PROTOCOL_BAD_WIRE_VERSION,
                         "The selected server does not support array filters");
         GOTO (done);
      }

      if (!mongoc_write_concern_is_acknowledged (
             update_opts->crud.writeConcern)) {
         bson_set_error (error,
                         MONGOC_ERROR_COMMAND,
                         MONGOC_ERROR_PROTOCOL_BAD_WIRE_VERSION,
                         "Cannot use array filters with unacknowledged writes");
         GOTO (done);
      }
   }

   if (_mongoc_client_session_in_txn (update_opts->crud.client_session) &&
       update_opts->crud.writeConcern) {
      bson_set_error (error,
                      MONGOC_ERROR_COMMAND,
                      MONGOC_ERROR_COMMAND_INVALID_ARG,
                      "Cannot set write concern after starting transaction");
      GOTO (done);
   }

   if (!update_opts->crud.writeConcern) {
      update_opts->crud.writeConcern = collection->write_concern;
      update_opts->crud.write_concern_owned = false;
   }

   _mongoc_write_command_execute_idl (&command,
                                      collection->client,
                                      server_stream,
                                      collection->db,
                                      collection->collection,
                                      0 /* offset */,
                                      &update_opts->crud,
                                      &result);

   _mongoc_bson_init_if_set (reply);
   reply_initialized = true;

   /* set fields described in CRUD spec for the UpdateResult */
   ret = MONGOC_WRITE_RESULT_COMPLETE (&result,
                                       collection->client->error_api_version,
                                       update_opts->crud.writeConcern,
                                       /* no error domain override */
                                       (mongoc_error_domain_t) 0,
                                       reply,
                                       error,
                                       "modifiedCount",
                                       "matchedCount",
                                       "upsertedCount",
                                       "upsertedId");

done:
   _mongoc_write_result_destroy (&result);
   mongoc_server_stream_cleanup (server_stream);
   _mongoc_write_command_destroy (&command);

   if (!reply_initialized) {
      _mongoc_bson_init_if_set (reply);
   }

   RETURN (ret);
}

bool
mongoc_collection_update_one (mongoc_collection_t *collection,
                              const bson_t *selector,
                              const bson_t *update,
                              const bson_t *opts,
                              bson_t *reply,
                              bson_error_t *error)
{
   mongoc_update_one_opts_t update_one_opts;
   bool ret;

   ENTRY;

   BSON_ASSERT (collection);
   BSON_ASSERT (update);

   if (!_mongoc_update_one_opts_parse (
          collection->client, opts, &update_one_opts, error)) {
      _mongoc_update_one_opts_cleanup (&update_one_opts);
      _mongoc_bson_init_if_set (reply);
      return false;
   }

   if (!_mongoc_validate_update (
          update, update_one_opts.update.crud.validate, error)) {
      _mongoc_update_one_opts_cleanup (&update_one_opts);
      _mongoc_bson_init_if_set (reply);
      return false;
   }

   ret = _mongoc_collection_update_or_replace (collection,
                                               selector,
                                               update,
                                               &update_one_opts.update,
                                               update_one_opts.update.bypass,
                                               &update_one_opts.arrayFilters,
                                               &update_one_opts.extra,
                                               reply,
                                               error);

   _mongoc_update_one_opts_cleanup (&update_one_opts);

   RETURN (ret);
}

bool
mongoc_collection_update_many (mongoc_collection_t *collection,
                               const bson_t *selector,
                               const bson_t *update,
                               const bson_t *opts,
                               bson_t *reply,
                               bson_error_t *error)
{
   mongoc_update_many_opts_t update_many_opts;
   bool ret;

   ENTRY;

   BSON_ASSERT (collection);
   BSON_ASSERT (update);

   if (!_mongoc_update_many_opts_parse (
          collection->client, opts, &update_many_opts, error)) {
      _mongoc_update_many_opts_cleanup (&update_many_opts);
      _mongoc_bson_init_if_set (reply);
      return false;
   }

   if (!_mongoc_validate_update (
          update, update_many_opts.update.crud.validate, error)) {
      _mongoc_update_many_opts_cleanup (&update_many_opts);
      _mongoc_bson_init_if_set (reply);
      return false;
   }

   bson_append_bool (&update_many_opts.extra, "multi", 5, true);

   ret = _mongoc_collection_update_or_replace (collection,
                                               selector,
                                               update,
                                               &update_many_opts.update,
                                               update_many_opts.update.bypass,
                                               &update_many_opts.arrayFilters,
                                               &update_many_opts.extra,
                                               reply,
                                               error);

   _mongoc_update_many_opts_cleanup (&update_many_opts);

   RETURN (ret);
}

bool
mongoc_collection_replace_one (mongoc_collection_t *collection,
                               const bson_t *selector,
                               const bson_t *replacement,
                               const bson_t *opts,
                               bson_t *reply,
                               bson_error_t *error)
{
   mongoc_replace_one_opts_t replace_one_opts;
   bool ret;

   ENTRY;

   BSON_ASSERT (collection);
   BSON_ASSERT (replacement);

   if (!_mongoc_replace_one_opts_parse (
          collection->client, opts, &replace_one_opts, error)) {
      _mongoc_replace_one_opts_cleanup (&replace_one_opts);
      _mongoc_bson_init_if_set (reply);
      return false;
   }

   if (!_mongoc_validate_replace (
          replacement, replace_one_opts.update.crud.validate, error)) {
      _mongoc_replace_one_opts_cleanup (&replace_one_opts);
      _mongoc_bson_init_if_set (reply);
      return false;
   }

   ret = _mongoc_collection_update_or_replace (collection,
                                               selector,
                                               replacement,
                                               &replace_one_opts.update,
                                               replace_one_opts.update.bypass,
                                               NULL,
                                               &replace_one_opts.extra,
                                               reply,
                                               error);

   _mongoc_replace_one_opts_cleanup (&replace_one_opts);

   RETURN (ret);
}


/*
 *--------------------------------------------------------------------------
 *
 * mongoc_collection_save --
 *
 *       Save @document to @collection.
 *
 *       If the document has an _id field, it will be updated. Otherwise,
 *       the document will be inserted into the collection.
 *
 * Returns:
 *       true if successful; otherwise false and @error is set.
 *
 * Side effects:
 *       @error is set upon failure if non-NULL.
 *
 *--------------------------------------------------------------------------
 */

bool
mongoc_collection_save (mongoc_collection_t *collection,
                        const bson_t *document,
                        const mongoc_write_concern_t *write_concern,
                        bson_error_t *error)
{
   bson_iter_t iter;
   bool ret;
   bson_t selector;

   BSON_ASSERT (collection);
   BSON_ASSERT (document);

   BEGIN_IGNORE_DEPRECATIONS
   if (!bson_iter_init_find (&iter, document, "_id")) {
      return mongoc_collection_insert (
         collection, MONGOC_INSERT_NONE, document, write_concern, error);
   }

   bson_init (&selector);
   if (!bson_append_iter (&selector, NULL, 0, &iter)) {
      bson_set_error (error,
                      MONGOC_ERROR_COMMAND,
                      MONGOC_ERROR_COMMAND_INVALID_ARG,
                      "Failed to append bson to create update.");
      bson_destroy (&selector);
      return false;
   }

   /* this document will be inserted, validate same as for inserts */
   if (!_mongoc_validate_new_document (
          document, _mongoc_default_insert_vflags, error)) {
      return false;
   }

   ret = mongoc_collection_update (collection,
                                   MONGOC_UPDATE_UPSERT |
                                      MONGOC_UPDATE_NO_VALIDATE,
                                   &selector,
                                   document,
                                   write_concern,
                                   error);
   END_IGNORE_DEPRECATIONS

   bson_destroy (&selector);

   return ret;
}


/*
 *--------------------------------------------------------------------------
 *
 * mongoc_collection_remove --
 *
 *       Delete one or more items from a collection. If you want to
 *       limit to a single delete, provided MONGOC_REMOVE_SINGLE_REMOVE
 *       for @flags.
 *
 *       Superseded by mongoc_collection_delete_one/many.
 *
 * Parameters:
 *       @collection: A mongoc_collection_t.
 *       @flags: the delete flags or 0.
 *       @selector: A selector of documents to delete.
 *       @write_concern: A write concern or NULL. If NULL, the default
 *                       write concern for the collection will be used.
 *       @error: A location for an error or NULL.
 *
 * Returns:
 *       true if successful; otherwise false and error is set.
 *
 *       If the write concern does not dictate checking the result, this
 *       function may return true even if it failed.
 *
 * Side effects:
 *       @collection->gle is setup, depending on write_concern->w value.
 *       @error is setup upon failure.
 *
 *--------------------------------------------------------------------------
 */

bool
mongoc_collection_remove (mongoc_collection_t *collection,
                          mongoc_remove_flags_t flags,
                          const bson_t *selector,
                          const mongoc_write_concern_t *write_concern,
                          bson_error_t *error)
{
   mongoc_bulk_write_flags_t write_flags = MONGOC_BULK_WRITE_FLAGS_INIT;
   mongoc_write_command_t command;
   mongoc_write_result_t result;
   bson_t opts;
   bool ret;

   ENTRY;

   BSON_ASSERT (collection);
   BSON_ASSERT (selector);

   bson_clear (&collection->gle);

   if (!write_concern) {
      write_concern = collection->write_concern;
   }

   bson_init (&opts);
   BSON_APPEND_INT32 (
      &opts, "limit", flags & MONGOC_REMOVE_SINGLE_REMOVE ? 1 : 0);
   _mongoc_write_result_init (&result);
   ++collection->client->cluster.operation_id;
   _mongoc_write_command_init_delete (&command,
                                      selector,
                                      NULL,
                                      &opts,
                                      write_flags,
                                      collection->client->cluster.operation_id);
   bson_destroy (&opts);

   _mongoc_collection_write_command_execute (
      &command, collection, write_concern, NULL, &result);

   collection->gle = bson_new ();
   ret = MONGOC_WRITE_RESULT_COMPLETE (&result,
                                       collection->client->error_api_version,
                                       write_concern,
                                       0 /* no error domain override */,
                                       collection->gle,
                                       error);

   _mongoc_write_result_destroy (&result);
   _mongoc_write_command_destroy (&command);

   RETURN (ret);
}


bool
mongoc_collection_delete (mongoc_collection_t *collection,
                          mongoc_delete_flags_t flags,
                          const bson_t *selector,
                          const mongoc_write_concern_t *write_concern,
                          bson_error_t *error)
{
   return mongoc_collection_remove (collection,
                                    (mongoc_remove_flags_t) flags,
                                    selector,
                                    write_concern,
                                    error);
}


static bool
_mongoc_delete_one_or_many (mongoc_collection_t *collection,
                            const bson_t *selector,
                            mongoc_crud_opts_t *crud,
                            const bson_t *cmd_opts,
                            const bson_t *collation,
                            bson_t *opts,
                            bson_t *reply,
                            bson_error_t *error)
{
   mongoc_write_command_t command;
   mongoc_write_result_t result;
   bool ret;

   ENTRY;

   BSON_ASSERT (collection);
   BSON_ASSERT (selector);
   BSON_ASSERT (bson_empty0 (reply));

   _mongoc_write_result_init (&result);

   if (!bson_empty (collation)) {
      bson_append_document (opts, "collation", 9, collation);
   }

   _mongoc_write_command_init_delete_idl (
      &command,
      selector,
      cmd_opts,
      opts,
      ++collection->client->cluster.operation_id);

   if (!bson_empty (collation)) {
      command.flags.has_collation = true;
   }

   _mongoc_collection_write_command_execute_idl (
      &command, collection, crud, &result);

   /* set field described in CRUD spec for the DeleteResult */
   ret = MONGOC_WRITE_RESULT_COMPLETE (&result,
                                       collection->client->error_api_version,
                                       crud->writeConcern,
                                       /* no error domain override */
                                       (mongoc_error_domain_t) 0,
                                       reply,
                                       error,
                                       "deletedCount");

   _mongoc_write_result_destroy (&result);
   _mongoc_write_command_destroy (&command);

   RETURN (ret);
}


bool
mongoc_collection_delete_one (mongoc_collection_t *collection,
                              const bson_t *selector,
                              const bson_t *opts,
                              bson_t *reply,
                              bson_error_t *error)
{
   mongoc_delete_one_opts_t delete_one_opts;
   bson_t limit = BSON_INITIALIZER;
   bool ret = false;

   ENTRY;

   BSON_ASSERT (collection);
   BSON_ASSERT (selector);

   _mongoc_bson_init_if_set (reply);
   bson_append_int32 (&limit, "limit", 5, 1);
   if (!_mongoc_delete_one_opts_parse (
          collection->client, opts, &delete_one_opts, error)) {
      GOTO (done);
   }

   ret = _mongoc_delete_one_or_many (collection,
                                     selector,
                                     &delete_one_opts.crud,
                                     &delete_one_opts.extra,
                                     &delete_one_opts.collation,
                                     &limit,
                                     reply,
                                     error);

done:
   _mongoc_delete_one_opts_cleanup (&delete_one_opts);
   bson_destroy (&limit);

   RETURN (ret);
}

bool
mongoc_collection_delete_many (mongoc_collection_t *collection,
                               const bson_t *selector,
                               const bson_t *opts,
                               bson_t *reply,
                               bson_error_t *error)
{
   mongoc_delete_many_opts_t delete_many_opts;
   bson_t limit = BSON_INITIALIZER;
   bool ret = false;

   ENTRY;

   BSON_ASSERT (collection);
   BSON_ASSERT (selector);

   _mongoc_bson_init_if_set (reply);
   if (!_mongoc_delete_many_opts_parse (
          collection->client, opts, &delete_many_opts, error)) {
      GOTO (done);
   }

   bson_append_int32 (&limit, "limit", 5, 0);
   ret = _mongoc_delete_one_or_many (collection,
                                     selector,
                                     &delete_many_opts.crud,
                                     &delete_many_opts.extra,
                                     &delete_many_opts.collation,
                                     &limit,
                                     reply,
                                     error);

done:
   _mongoc_delete_many_opts_cleanup (&delete_many_opts);
   bson_destroy (&limit);

   RETURN (ret);
}


/*
 *--------------------------------------------------------------------------
 *
 * mongoc_collection_get_read_prefs --
 *
 *       Fetch the default read preferences for the collection.
 *
 * Returns:
 *       A mongoc_read_prefs_t that should not be modified or freed.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

const mongoc_read_prefs_t *
mongoc_collection_get_read_prefs (const mongoc_collection_t *collection)
{
   BSON_ASSERT (collection);
   return collection->read_prefs;
}


/*
 *--------------------------------------------------------------------------
 *
 * mongoc_collection_set_read_prefs --
 *
 *       Sets the default read preferences for the collection instance.
 *
 * Returns:
 *       None.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

void
mongoc_collection_set_read_prefs (mongoc_collection_t *collection,
                                  const mongoc_read_prefs_t *read_prefs)
{
   BSON_ASSERT (collection);

   if (collection->read_prefs) {
      mongoc_read_prefs_destroy (collection->read_prefs);
      collection->read_prefs = NULL;
   }

   if (read_prefs) {
      collection->read_prefs = mongoc_read_prefs_copy (read_prefs);
   }
}


/*
 *--------------------------------------------------------------------------
 *
 * mongoc_collection_get_read_concern --
 *
 *       Fetches the default read concern for the collection instance.
 *
 * Returns:
 *       A mongoc_read_concern_t that should not be modified or freed.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

const mongoc_read_concern_t *
mongoc_collection_get_read_concern (const mongoc_collection_t *collection)
{
   BSON_ASSERT (collection);

   return collection->read_concern;
}


/*
 *--------------------------------------------------------------------------
 *
 * mongoc_collection_set_read_concern --
 *
 *       Sets the default read concern for the collection instance.
 *
 * Returns:
 *       None.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

void
mongoc_collection_set_read_concern (mongoc_collection_t *collection,
                                    const mongoc_read_concern_t *read_concern)
{
   BSON_ASSERT (collection);

   if (collection->read_concern) {
      mongoc_read_concern_destroy (collection->read_concern);
      collection->read_concern = NULL;
   }

   if (read_concern) {
      collection->read_concern = mongoc_read_concern_copy (read_concern);
   }
}


/*
 *--------------------------------------------------------------------------
 *
 * mongoc_collection_get_write_concern --
 *
 *       Fetches the default write concern for the collection instance.
 *
 * Returns:
 *       A mongoc_write_concern_t that should not be modified or freed.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

const mongoc_write_concern_t *
mongoc_collection_get_write_concern (const mongoc_collection_t *collection)
{
   BSON_ASSERT (collection);

   return collection->write_concern;
}


/*
 *--------------------------------------------------------------------------
 *
 * mongoc_collection_set_write_concern --
 *
 *       Sets the default write concern for the collection instance.
 *
 * Returns:
 *       None.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

void
mongoc_collection_set_write_concern (
   mongoc_collection_t *collection, const mongoc_write_concern_t *write_concern)
{
   BSON_ASSERT (collection);

   if (collection->write_concern) {
      mongoc_write_concern_destroy (collection->write_concern);
      collection->write_concern = NULL;
   }

   if (write_concern) {
      collection->write_concern = mongoc_write_concern_copy (write_concern);
   }
}


/*
 *--------------------------------------------------------------------------
 *
 * mongoc_collection_get_name --
 *
 *       Returns the name of the collection, excluding the database name.
 *
 * Returns:
 *       A string which should not be modified or freed.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

const char *
mongoc_collection_get_name (mongoc_collection_t *collection)
{
   BSON_ASSERT (collection);

   return collection->collection;
}


/*
 *--------------------------------------------------------------------------
 *
 * mongoc_collection_get_last_error --
 *
 *       Returns a bulk result.
 *
 * Returns:
 *       NULL or a bson_t that should not be modified or freed. This value
 *       is not guaranteed to be persistent between calls into the
 *       mongoc_collection_t instance, and therefore must be copied if
 *       you would like to keep it around.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

const bson_t *
mongoc_collection_get_last_error (
   const mongoc_collection_t *collection) /* IN */
{
   BSON_ASSERT (collection);

   return collection->gle;
}


/*
 *--------------------------------------------------------------------------
 *
 * mongoc_collection_validate --
 *
 *       Helper to call the validate command on the MongoDB server to
 *       validate the collection.
 *
 *       Options may be additional options, or NULL.
 *       Currently supported options are:
 *
 *          "full": Boolean
 *
 *       If full is true, then perform a more resource intensive
 *       validation.
 *
 *       The result is stored in reply.
 *
 * Returns:
 *       true if successful; otherwise false and @error is set.
 *
 * Side effects:
 *       @reply is set if successful.
 *       @error may be set.
 *
 *--------------------------------------------------------------------------
 */

bool
mongoc_collection_validate (mongoc_collection_t *collection, /* IN */
                            const bson_t *options,           /* IN */
                            bson_t *reply,                   /* OUT */
                            bson_error_t *error)             /* IN */
{
   bson_iter_t iter;
   bson_t cmd = BSON_INITIALIZER;
   bool ret = false;
   bool reply_initialized = false;

   BSON_ASSERT (collection);

   if (options && bson_iter_init_find (&iter, options, "full") &&
       !BSON_ITER_HOLDS_BOOL (&iter)) {
      bson_set_error (error,
                      MONGOC_ERROR_BSON,
                      MONGOC_ERROR_BSON_INVALID,
                      "'full' must be a boolean value.");
      goto cleanup;
   }

   bson_append_utf8 (
      &cmd, "validate", 8, collection->collection, collection->collectionlen);

   if (options) {
      bson_concat (&cmd, options);
   }

   ret =
      mongoc_collection_command_simple (collection, &cmd, NULL, reply, error);
   reply_initialized = true;

cleanup:
   bson_destroy (&cmd);

   if (reply && !reply_initialized) {
      bson_init (reply);
   }

   return ret;
}


/*
 *--------------------------------------------------------------------------
 *
 * mongoc_collection_rename --
 *
 *       Rename the collection to @new_name.
 *
 *       If @new_db is NULL, the same db will be used.
 *
 *       If @drop_target_before_rename is true, then a collection named
 *       @new_name will be dropped before renaming @collection to
 *       @new_name.
 *
 * Returns:
 *       true on success; false on failure and @error is set.
 *
 * Side effects:
 *       @error is set on failure.
 *
 *--------------------------------------------------------------------------
 */

bool
mongoc_collection_rename (mongoc_collection_t *collection,
                          const char *new_db,
                          const char *new_name,
                          bool drop_target_before_rename,
                          bson_error_t *error)
{
   return mongoc_collection_rename_with_opts (
      collection, new_db, new_name, drop_target_before_rename, NULL, error);
}


bool
mongoc_collection_rename_with_opts (mongoc_collection_t *collection,
                                    const char *new_db,
                                    const char *new_name,
                                    bool drop_target_before_rename,
                                    const bson_t *opts,
                                    bson_error_t *error)
{
   bson_t cmd = BSON_INITIALIZER;
   char newns[MONGOC_NAMESPACE_MAX + 1];
   bool ret;

   BSON_ASSERT (collection);
   BSON_ASSERT (new_name);

   if (strchr (new_name, '$')) {
      bson_set_error (error,
                      MONGOC_ERROR_NAMESPACE,
                      MONGOC_ERROR_NAMESPACE_INVALID,
                      "\"%s\" is an invalid collection name.",
                      new_name);
      return false;
   }

   bson_snprintf (
      newns, sizeof newns, "%s.%s", new_db ? new_db : collection->db, new_name);

   BSON_APPEND_UTF8 (&cmd, "renameCollection", collection->ns);
   BSON_APPEND_UTF8 (&cmd, "to", newns);

   if (drop_target_before_rename) {
      BSON_APPEND_BOOL (&cmd, "dropTarget", true);
   }

   ret = _mongoc_client_command_with_opts (collection->client,
                                           "admin",
                                           &cmd,
                                           MONGOC_CMD_WRITE,
                                           opts,
                                           MONGOC_QUERY_NONE,
                                           NULL, /* user prefs */
                                           collection->read_prefs,
                                           collection->read_concern,
                                           collection->write_concern,
                                           NULL, /* reply */
                                           error);

   if (ret) {
      if (new_db) {
         bson_snprintf (collection->db, sizeof collection->db, "%s", new_db);
      }

      bson_snprintf (
         collection->collection, sizeof collection->collection, "%s", new_name);
      collection->collectionlen = (int) strlen (collection->collection);

      bson_snprintf (collection->ns,
                     sizeof collection->ns,
                     "%s.%s",
                     collection->db,
                     new_name);
      collection->nslen = (int) strlen (collection->ns);
   }

   bson_destroy (&cmd);

   return ret;
}


/*
 *--------------------------------------------------------------------------
 *
 * mongoc_collection_stats --
 *
 *       Fetches statistics about the collection.
 *
 *       The result is stored in @stats, which should NOT be an initialized
 *       bson_t or a leak will occur.
 *
 *       @stats, @options, and @error are optional.
 *
 * Returns:
 *       true on success and @stats is set.
 *       false on failure and @error is set.
 *
 * Side effects:
 *       @stats and @error.
 *
 *--------------------------------------------------------------------------
 */

bool
mongoc_collection_stats (mongoc_collection_t *collection,
                         const bson_t *options,
                         bson_t *stats,
                         bson_error_t *error)
{
   bson_iter_t iter;
   bson_t cmd = BSON_INITIALIZER;
   bool ret;

   BSON_ASSERT (collection);

   if (options && bson_iter_init_find (&iter, options, "scale") &&
       !BSON_ITER_HOLDS_INT32 (&iter)) {
      bson_set_error (error,
                      MONGOC_ERROR_BSON,
                      MONGOC_ERROR_BSON_INVALID,
                      "'scale' must be an int32 value.");
      return false;
   }

   BSON_APPEND_UTF8 (&cmd, "collStats", collection->collection);

   if (options) {
      bson_concat (&cmd, options);
   }

   /* Server Selection Spec: "may-use-secondary" commands SHOULD take a read
    * preference argument and otherwise MUST use the default read preference
    * from client, database or collection configuration. */
   ret = mongoc_collection_command_simple (
      collection, &cmd, collection->read_prefs, stats, error);

   bson_destroy (&cmd);

   return ret;
}


mongoc_bulk_operation_t *
mongoc_collection_create_bulk_operation (
   mongoc_collection_t *collection,
   bool ordered,
   const mongoc_write_concern_t *write_concern)
{
   bson_t opts = BSON_INITIALIZER;
   mongoc_bulk_operation_t *bulk;
   bool wc_ok = true;

   bson_append_bool (&opts, "ordered", 7, ordered);
   if (write_concern) {
      wc_ok = mongoc_write_concern_append (
         (mongoc_write_concern_t *) write_concern, &opts);
   }

   bulk = mongoc_collection_create_bulk_operation_with_opts (collection, &opts);

   bson_destroy (&opts);

   if (!wc_ok) {
      bson_set_error (&bulk->result.error,
                      MONGOC_ERROR_COMMAND,
                      MONGOC_ERROR_COMMAND_INVALID_ARG,
                      "invalid writeConcern");
   }

   return bulk;
}


mongoc_bulk_operation_t *
mongoc_collection_create_bulk_operation_with_opts (
   mongoc_collection_t *collection, const bson_t *opts)
{
   mongoc_bulk_opts_t bulk_opts;
   mongoc_bulk_write_flags_t write_flags = MONGOC_BULK_WRITE_FLAGS_INIT;
   mongoc_write_concern_t *wc;
   mongoc_bulk_operation_t *bulk;
   bson_error_t err = {0};

   BSON_ASSERT (collection);

   (void) _mongoc_bulk_opts_parse (collection->client, opts, &bulk_opts, &err);
   wc = COALESCE (bulk_opts.writeConcern, collection->write_concern);
   write_flags.ordered = bulk_opts.ordered;
   bulk = _mongoc_bulk_operation_new (collection->client,
                                      collection->db,
                                      collection->collection,
                                      write_flags,
                                      wc);

   bulk->session = bulk_opts.client_session;
   if (err.domain) {
      /* _mongoc_bulk_opts_parse failed, above */
      memcpy (&bulk->result.error, &err, sizeof (bson_error_t));
   } else if (_mongoc_client_session_in_txn (bulk_opts.client_session) &&
              !mongoc_write_concern_is_default (bulk_opts.writeConcern)) {
      bson_set_error (&bulk->result.error,
                      MONGOC_ERROR_COMMAND,
                      MONGOC_ERROR_COMMAND_INVALID_ARG,
                      "Cannot set write concern after starting transaction");
   }

   _mongoc_bulk_opts_cleanup (&bulk_opts);

   return bulk;
}

/*
 *--------------------------------------------------------------------------
 *
 * mongoc_collection_find_and_modify_with_opts --
 *
 *       Find a document in @collection matching @query, applying @opts.
 *
 *       If @reply is not NULL, then the result document will be placed
 *       in reply and should be released with bson_destroy().
 *
 *       See http://docs.mongodb.org/manual/reference/command/findAndModify/
 *       for more information.
 *
 * Returns:
 *       true on success; false on failure.
 *
 * Side effects:
 *       reply is initialized.
 *       error is set if false is returned.
 *
 *--------------------------------------------------------------------------
 */
bool
mongoc_collection_find_and_modify_with_opts (
   mongoc_collection_t *collection,
   const bson_t *query,
   const mongoc_find_and_modify_opts_t *opts,
   bson_t *reply,
   bson_error_t *error)
{
   mongoc_cluster_t *cluster;
   mongoc_client_session_t *cs = NULL;
   mongoc_cmd_parts_t parts;
   bool is_retryable;
   bson_iter_t iter;
   bson_iter_t inner;
   const char *name;
   bson_t reply_local;
   bson_t *reply_ptr;
   bool ret = false;
   bson_t command = BSON_INITIALIZER;
   mongoc_server_stream_t *server_stream = NULL;
   mongoc_server_stream_t *retry_server_stream = NULL;

   ENTRY;

   BSON_ASSERT (collection);
   BSON_ASSERT (query);

   reply_ptr = reply ? reply : &reply_local;
   cluster = &collection->client->cluster;

   mongoc_cmd_parts_init (
      &parts, collection->client, collection->db, MONGOC_QUERY_NONE, &command);
   parts.is_read_command = true;
   parts.is_write_command = true;

   /* we need a session to fetch a stream to call cmd_parts_append_opts
    * below, which parses the session from opts->extra *again*
    */
   if (bson_iter_init_find (&iter, &opts->extra, "sessionId")) {
      if (!_mongoc_client_session_from_iter (
             collection->client, &iter, &cs, error)) {
         bson_init (reply_ptr);
         GOTO (done);
      }
   }

   server_stream =
      mongoc_cluster_stream_for_writes (cluster, cs, reply_ptr, error);

   if (!server_stream) {
      GOTO (done);
   }

   bson_init (reply_ptr);
   name = mongoc_collection_get_name (collection);
   BSON_APPEND_UTF8 (&command, "findAndModify", name);
   BSON_APPEND_DOCUMENT (&command, "query", query);

   if (opts->sort) {
      BSON_APPEND_DOCUMENT (&command, "sort", opts->sort);
   }

   if (opts->update) {
      BSON_APPEND_DOCUMENT (&command, "update", opts->update);
   }

   if (opts->fields) {
      BSON_APPEND_DOCUMENT (&command, "fields", opts->fields);
   }

   if (opts->flags & MONGOC_FIND_AND_MODIFY_REMOVE) {
      BSON_APPEND_BOOL (&command, "remove", true);
   }

   if (opts->flags & MONGOC_FIND_AND_MODIFY_UPSERT) {
      BSON_APPEND_BOOL (&command, "upsert", true);
   }

   if (opts->flags & MONGOC_FIND_AND_MODIFY_RETURN_NEW) {
      BSON_APPEND_BOOL (&command, "new", true);
   }

   if (opts->bypass_document_validation !=
       MONGOC_BYPASS_DOCUMENT_VALIDATION_DEFAULT) {
      BSON_APPEND_BOOL (&command,
                        "bypassDocumentValidation",
                        !!opts->bypass_document_validation);
   }

   if (opts->max_time_ms > 0) {
      BSON_APPEND_INT32 (&command, "maxTimeMS", opts->max_time_ms);
   }

   if (bson_iter_init (&iter, &opts->extra)) {
      bool ok = mongoc_cmd_parts_append_opts (
         &parts, &iter, server_stream->sd->max_wire_version, error);
      if (!ok) {
         GOTO (done);
      }
   }

   if (_mongoc_client_session_in_txn (parts.assembled.session) &&
       !bson_empty (&parts.write_concern_document)) {
      bson_set_error (error,
                      MONGOC_ERROR_COMMAND,
                      MONGOC_ERROR_COMMAND_INVALID_ARG,
                      "Cannot set write concern after starting transaction");
      GOTO (done);
   }

   if (!bson_has_field (&opts->extra, "writeConcern")) {
      if (server_stream->sd->max_wire_version >=
          WIRE_VERSION_FAM_WRITE_CONCERN) {
         if (!mongoc_write_concern_is_valid (collection->write_concern)) {
            bson_set_error (error,
                            MONGOC_ERROR_COMMAND,
                            MONGOC_ERROR_COMMAND_INVALID_ARG,
                            "The write concern is invalid.");
            GOTO (done);
         }

         if (mongoc_write_concern_is_acknowledged (collection->write_concern)) {
            if (!mongoc_cmd_parts_set_write_concern (
                   &parts,
                   collection->write_concern,
                   server_stream->sd->max_wire_version,
                   error)) {
               GOTO (done);
            }
         }
      }
   }

   parts.assembled.operation_id = ++cluster->operation_id;
   if (!mongoc_cmd_parts_assemble (&parts, server_stream, error)) {
      GOTO (done);
   }

   is_retryable = parts.is_retryable_write;

   /* increment the transaction number for the first attempt of each retryable
    * write command */
   if (is_retryable) {
      bson_iter_t txn_number_iter;
      BSON_ASSERT (bson_iter_init_find (
         &txn_number_iter, parts.assembled.command, "txnNumber"));
      bson_iter_overwrite_int64 (
         &txn_number_iter,
         ++parts.assembled.session->server_session->txn_number);
   }
retry:
   bson_destroy (reply_ptr);
   ret = mongoc_cluster_run_command_monitored (
      cluster, &parts.assembled, reply_ptr, error);

   /* If a retryable error is encountered and the write is retryable, select
    * a new writable stream and retry. If server selection fails or the selected
    * server does not support retryable writes, fall through and allow the
    * original error to be reported. */
   if (!ret && is_retryable &&
       (error->domain == MONGOC_ERROR_STREAM ||
        _mongoc_write_error_get_type (reply_ptr) == MONGOC_WRITE_ERR_RETRY)) {
      bson_error_t ignored_error;

      /* each write command may be retried at most once */
      is_retryable = false;
      retry_server_stream = mongoc_cluster_stream_for_writes (
         cluster, parts.assembled.session, NULL /* reply */, &ignored_error);

      if (retry_server_stream &&
          retry_server_stream->sd->max_wire_version >=
             WIRE_VERSION_RETRY_WRITES) {
         parts.assembled.server_stream = retry_server_stream;
         GOTO (retry);
      }
   }

   if (bson_iter_init_find (&iter, reply_ptr, "writeConcernError") &&
       BSON_ITER_HOLDS_DOCUMENT (&iter)) {
      const char *errmsg = NULL;
      int32_t code = 0;

      BSON_ASSERT (bson_iter_recurse (&iter, &inner));
      while (bson_iter_next (&inner)) {
         if (BSON_ITER_IS_KEY (&inner, "code")) {
            code = bson_iter_int32 (&inner);
         } else if (BSON_ITER_IS_KEY (&inner, "errmsg")) {
            errmsg = bson_iter_utf8 (&inner, NULL);
         }
      }
      bson_set_error (error,
                      MONGOC_ERROR_WRITE_CONCERN,
                      code,
                      "Write Concern error: %s",
                      errmsg);
      ret = false;
   }


done:
   mongoc_server_stream_cleanup (server_stream);
   mongoc_server_stream_cleanup (retry_server_stream);
   mongoc_cmd_parts_cleanup (&parts);
   bson_destroy (&command);
   if (&reply_local == reply_ptr) {
      bson_destroy (&reply_local);
   }
   RETURN (ret);
}

/*
 *--------------------------------------------------------------------------
 *
 * mongoc_collection_find_and_modify --
 *
 *       Find a document in @collection matching @query and update it with
 *       the update document @update.
 *
 *       If @reply is not NULL, then the result document will be placed
 *       in reply and should be released with bson_destroy().
 *
 *       If @remove is true, then the matching documents will be removed.
 *
 *       If @fields is not NULL, it will be used to select the desired
 *       resulting fields.
 *
 *       If @_new is true, then the new version of the document is returned
 *       instead of the old document.
 *
 *       See http://docs.mongodb.org/manual/reference/command/findAndModify/
 *       for more information.
 *
 * Returns:
 *       true on success; false on failure.
 *
 * Side effects:
 *       reply is initialized.
 *       error is set if false is returned.
 *
 *--------------------------------------------------------------------------
 */

bool
mongoc_collection_find_and_modify (mongoc_collection_t *collection,
                                   const bson_t *query,
                                   const bson_t *sort,
                                   const bson_t *update,
                                   const bson_t *fields,
                                   bool _remove,
                                   bool upsert,
                                   bool _new,
                                   bson_t *reply,
                                   bson_error_t *error)
{
   mongoc_find_and_modify_opts_t *opts;
   int flags = 0;
   bool ret;

   ENTRY;

   BSON_ASSERT (collection);
   BSON_ASSERT (query);
   BSON_ASSERT (update || _remove);


   if (_remove) {
      flags |= MONGOC_FIND_AND_MODIFY_REMOVE;
   }
   if (upsert) {
      flags |= MONGOC_FIND_AND_MODIFY_UPSERT;
   }
   if (_new) {
      flags |= MONGOC_FIND_AND_MODIFY_RETURN_NEW;
   }

   opts = mongoc_find_and_modify_opts_new ();

   mongoc_find_and_modify_opts_set_sort (opts, sort);
   mongoc_find_and_modify_opts_set_update (opts, update);
   mongoc_find_and_modify_opts_set_fields (opts, fields);
   mongoc_find_and_modify_opts_set_flags (opts, flags);

   ret = mongoc_collection_find_and_modify_with_opts (
      collection, query, opts, reply, error);
   mongoc_find_and_modify_opts_destroy (opts);

   return ret;
}

mongoc_change_stream_t *
mongoc_collection_watch (const mongoc_collection_t *coll,
                         const bson_t *pipeline,
                         const bson_t *opts)
{
   return _mongoc_change_stream_new_from_collection (coll, pipeline, opts);
}