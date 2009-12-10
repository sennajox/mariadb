/* Copyright (C) 2000-2004 MySQL AB
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */


/* HANDLER ... commands - direct access to ISAM */

/* TODO:
  HANDLER blabla OPEN [ AS foobar ] [ (column-list) ]

  the most natural (easiest, fastest) way to do it is to
  compute List<Item> field_list not in mysql_ha_read
  but in mysql_ha_open, and then store it in TABLE structure.

  The problem here is that mysql_parse calls free_item to free all the
  items allocated at the end of every query. The workaround would to
  keep two item lists per THD - normal free_list and handler_items.
  The second is to be freeed only on thread end. mysql_ha_open should
  then do { handler_items=concat(handler_items, free_list); free_list=0; }

  But !!! do_command calls free_root at the end of every query and frees up
  all the sql_alloc'ed memory. It's harder to work around...
*/

/*
  There are two containers holding information about open handler tables.
  The first is 'thd->handler_tables'. It is a linked list of TABLE objects.
  It is used like 'thd->open_tables' in the table cache. The trick is to
  exchange these two lists during open and lock of tables. Thus the normal
  table cache code can be used.
  The second container is a HASH. It holds objects of the type TABLE_LIST.
  Despite its name, no lists of tables but only single structs are hashed
  (the 'next' pointer is always NULL). The reason for theis second container
  is, that we want handler tables to survive FLUSH TABLE commands. A table
  affected by FLUSH TABLE must be closed so that other threads are not
  blocked by handler tables still in use. Since we use the normal table cache
  functions with 'thd->handler_tables', the closed tables are removed from
  this list. Hence we need the original open information for the handler
  table in the case that it is used again. This information is handed over
  to mysql_ha_open() as a TABLE_LIST. So we store this information in the
  second container, where it is not affected by FLUSH TABLE. The second
  container is implemented as a hash for performance reasons. Consequently,
  we use it not only for re-opening a handler table, but also for the
  HANDLER ... READ commands. For this purpose, we store a pointer to the
  TABLE structure (in the first container) in the TBALE_LIST object in the
  second container. When the table is flushed, the pointer is cleared.
*/

#include "mysql_priv.h"
#include "sql_select.h"
#include <assert.h>

#define HANDLER_TABLES_HASH_SIZE 120

static enum enum_ha_read_modes rkey_to_rnext[]=
{ RNEXT_SAME, RNEXT, RPREV, RNEXT, RPREV, RNEXT, RPREV, RPREV };

/*
  Get hash key and hash key length.

  SYNOPSIS
    mysql_ha_hash_get_key()
    tables                      Pointer to the hash object.
    key_len_p   (out)           Pointer to the result for key length.
    first                       Unused.

  DESCRIPTION
    The hash object is an TABLE_LIST struct.
    The hash key is the alias name.
    The hash key length is the alias name length plus one for the
    terminateing NUL character.

  RETURN
    Pointer to the TABLE_LIST struct.
*/

static char *mysql_ha_hash_get_key(TABLE_LIST *tables, size_t *key_len_p,
                                   my_bool first __attribute__((unused)))
{
  *key_len_p= strlen(tables->alias) + 1 ; /* include '\0' in comparisons */
  return tables->alias;
}


/*
  Free an hash object.

  SYNOPSIS
    mysql_ha_hash_free()
    tables                      Pointer to the hash object.

  DESCRIPTION
    The hash object is an TABLE_LIST struct.

  RETURN
    Nothing
*/

static void mysql_ha_hash_free(TABLE_LIST *tables)
{
  my_free((char*) tables, MYF(0));
}

/**
  Close a HANDLER table.

  @param thd Thread identifier.
  @param tables A list of tables with the first entry to close.

  @note Though this function takes a list of tables, only the first list entry
  will be closed.
  @note Broadcasts refresh if it closed a table with old version.
*/

static void mysql_ha_close_table(THD *thd, TABLE_LIST *tables)
{
  TABLE **table_ptr;
  MDL_ticket *mdl_ticket;

  /*
    Though we could take the table pointer from hash_tables->table,
    we must follow the thd->handler_tables chain anyway, as we need the
    address of the 'next' pointer referencing this table
    for close_thread_table().
  */
  for (table_ptr= &(thd->handler_tables);
       *table_ptr && (*table_ptr != tables->table);
         table_ptr= &(*table_ptr)->next)
    ;

  if (*table_ptr)
  {
    (*table_ptr)->file->ha_index_or_rnd_end();
    mdl_ticket= (*table_ptr)->mdl_ticket;
    pthread_mutex_lock(&LOCK_open);
    if (close_thread_table(thd, table_ptr))
    {
      /* Tell threads waiting for refresh that something has happened */
      broadcast_refresh();
    }
    pthread_mutex_unlock(&LOCK_open);
    thd->handler_mdl_context.release_lock(mdl_ticket);
  }
  else if (tables->table)
  {
    /* Must be a temporary table */
    TABLE *table= tables->table;
    table->file->ha_index_or_rnd_end();
    table->query_id= thd->query_id;
    table->open_by_handler= 0;
  }

  /* Mark table as closed, ready for re-open if necessary. */
  tables->table= NULL;
  /* Safety, cleanup the pointer to satisfy MDL assertions. */
  tables->mdl_request.ticket= NULL;
}

/*
  Open a HANDLER table.

  SYNOPSIS
    mysql_ha_open()
    thd                         Thread identifier.
    tables                      A list of tables with the first entry to open.
    reopen                      Re-open a previously opened handler table.

  DESCRIPTION
    Though this function takes a list of tables, only the first list entry
    will be opened.
    'reopen' is set when a handler table is to be re-opened. In this case,
    'tables' is the pointer to the hashed TABLE_LIST object which has been
    saved on the original open.
    'reopen' is also used to suppress the sending of an 'ok' message.

  RETURN
    FALSE OK
    TRUE  Error
*/

bool mysql_ha_open(THD *thd, TABLE_LIST *tables, bool reopen)
{
  TABLE_LIST    *hash_tables = NULL;
  char          *db, *name, *alias;
  uint          dblen, namelen, aliaslen, counter;
  bool          error;
  TABLE         *backup_open_tables;
  MDL_context   backup_mdl_context;
  DBUG_ENTER("mysql_ha_open");
  DBUG_PRINT("enter",("'%s'.'%s' as '%s'  reopen: %d",
                      tables->db, tables->table_name, tables->alias,
                      (int) reopen));

  if (thd->locked_tables_mode)
  {
    my_error(ER_LOCK_OR_ACTIVE_TRANSACTION, MYF(0));
    DBUG_RETURN(TRUE);
  }
  if (tables->schema_table)
  {
    my_error(ER_WRONG_USAGE, MYF(0), "HANDLER OPEN",
             INFORMATION_SCHEMA_NAME.str);
    DBUG_PRINT("exit",("ERROR"));
    DBUG_RETURN(TRUE);
  }

  if (! my_hash_inited(&thd->handler_tables_hash))
  {
    /*
      HASH entries are of type TABLE_LIST.
    */
    if (my_hash_init(&thd->handler_tables_hash, &my_charset_latin1,
                     HANDLER_TABLES_HASH_SIZE, 0, 0,
                     (my_hash_get_key) mysql_ha_hash_get_key,
                     (my_hash_free_key) mysql_ha_hash_free, 0))
    {
      DBUG_PRINT("exit",("ERROR"));
      DBUG_RETURN(TRUE);
    }
  }
  else if (! reopen) /* Otherwise we have 'tables' already. */
  {
    if (my_hash_search(&thd->handler_tables_hash, (uchar*) tables->alias,
                       strlen(tables->alias) + 1))
    {
      DBUG_PRINT("info",("duplicate '%s'", tables->alias));
      DBUG_PRINT("exit",("ERROR"));
      my_error(ER_NONUNIQ_TABLE, MYF(0), tables->alias);
      DBUG_RETURN(TRUE);
    }
  }

  if (! reopen)
  {
    /* copy the TABLE_LIST struct */
    dblen= strlen(tables->db) + 1;
    namelen= strlen(tables->table_name) + 1;
    aliaslen= strlen(tables->alias) + 1;
    if (!(my_multi_malloc(MYF(MY_WME),
                          &hash_tables, (uint) sizeof(*hash_tables),
                          &db, (uint) dblen,
                          &name, (uint) namelen,
                          &alias, (uint) aliaslen,
                          NullS)))
    {
      DBUG_PRINT("exit",("ERROR"));
      DBUG_RETURN(TRUE);
    }
    /* structure copy */
    *hash_tables= *tables;
    hash_tables->db= db;
    hash_tables->table_name= name;
    hash_tables->alias= alias;
    memcpy(hash_tables->db, tables->db, dblen);
    memcpy(hash_tables->table_name, tables->table_name, namelen);
    memcpy(hash_tables->alias, tables->alias, aliaslen);
    hash_tables->mdl_request.init(MDL_key::TABLE, db, name, MDL_SHARED);

    /* add to hash */
    if (my_hash_insert(&thd->handler_tables_hash, (uchar*) hash_tables))
    {
      my_free((char*) hash_tables, MYF(0));
      DBUG_PRINT("exit",("ERROR"));
      DBUG_RETURN(TRUE);
    }
  }
  else
    hash_tables= tables;

  /*
    Save and reset the open_tables list so that open_tables() won't
    be able to access (or know about) the previous list. And on return
    from open_tables(), thd->open_tables will contain only the opened
    table.

    The thd->handler_tables list is kept as-is to avoid deadlocks if
    open_table(), called by open_tables(), needs to back-off because
    of a pending exclusive metadata lock or flush for the table being
    opened.

    See open_table() back-off comments for more details.
  */
  backup_open_tables= thd->open_tables;
  thd->open_tables= NULL;
  thd->mdl_context.backup_and_reset(&backup_mdl_context);

  /*
    open_tables() will set 'hash_tables->table' if successful.
    It must be NULL for a real open when calling open_tables().
  */
  DBUG_ASSERT(! hash_tables->table);

  /* for now HANDLER can be used only for real TABLES */
  hash_tables->required_type= FRMTYPE_TABLE;
  /*
    We use open_tables() here, rather than, say,
    open_ltable() or open_table() because we would like to be able
    to open a temporary table.
  */
  error= open_tables(thd, &hash_tables, &counter, 0);
  if (thd->open_tables)
  {
    if (thd->open_tables->next)
    {
      /*
        We opened something that is more than a single table.
        This happens with MERGE engine. Don't try to link
        this mess into thd->handler_tables list, close it
        and report an error. We must do it right away
        because mysql_ha_close_table(), called down the road,
        can close a single table only.
      */
      close_thread_tables(thd);
      thd->mdl_context.release_all_locks();
      my_error(ER_ILLEGAL_HA, MYF(0), hash_tables->alias);
      error= TRUE;
    }
    else
    {
      /* Merge the opened table into handler_tables list. */
      thd->open_tables->next= thd->handler_tables;
      thd->handler_tables= thd->open_tables;
    }
  }
  thd->handler_mdl_context.merge(&thd->mdl_context);

  thd->open_tables= backup_open_tables;
  thd->mdl_context.restore_from_backup(&backup_mdl_context);

  if (error)
    goto err;

  /* There can be only one table in '*tables'. */
  if (! (hash_tables->table->file->ha_table_flags() & HA_CAN_SQL_HANDLER))
  {
    my_error(ER_ILLEGAL_HA, MYF(0), tables->alias);
    goto err;
  }

  /*
    If it's a temp table, don't reset table->query_id as the table is
    being used by this handler. Otherwise, no meaning at all.
  */
  hash_tables->table->open_by_handler= 1;

  if (! reopen)
    my_ok(thd);
  DBUG_PRINT("exit",("OK"));
  DBUG_RETURN(FALSE);

err:
  if (hash_tables->table)
    mysql_ha_close_table(thd, hash_tables);
  if (!reopen)
    my_hash_delete(&thd->handler_tables_hash, (uchar*) hash_tables);
  DBUG_PRINT("exit",("ERROR"));
  DBUG_RETURN(TRUE);
}


/*
  Close a HANDLER table by alias or table name

  SYNOPSIS
    mysql_ha_close()
    thd                         Thread identifier.
    tables                      A list of tables with the first entry to close.

  DESCRIPTION
    Closes the table that is associated (on the handler tables hash) with the
    name (table->alias) of the specified table.

  RETURN
    FALSE ok
    TRUE  error
*/

bool mysql_ha_close(THD *thd, TABLE_LIST *tables)
{
  TABLE_LIST    *hash_tables;
  DBUG_ENTER("mysql_ha_close");
  DBUG_PRINT("enter",("'%s'.'%s' as '%s'",
                      tables->db, tables->table_name, tables->alias));

  if (thd->locked_tables_mode)
  {
    my_error(ER_LOCK_OR_ACTIVE_TRANSACTION, MYF(0));
    DBUG_RETURN(TRUE);
  }
  if ((hash_tables= (TABLE_LIST*) my_hash_search(&thd->handler_tables_hash,
                                                 (uchar*) tables->alias,
                                                 strlen(tables->alias) + 1)))
  {
    mysql_ha_close_table(thd, hash_tables);
    my_hash_delete(&thd->handler_tables_hash, (uchar*) hash_tables);
  }
  else
  {
    my_error(ER_UNKNOWN_TABLE, MYF(0), tables->alias, "HANDLER");
    DBUG_PRINT("exit",("ERROR"));
    DBUG_RETURN(TRUE);
  }

  my_ok(thd);
  DBUG_PRINT("exit", ("OK"));
  DBUG_RETURN(FALSE);
}


/*
  Read from a HANDLER table.

  SYNOPSIS
    mysql_ha_read()
    thd                         Thread identifier.
    tables                      A list of tables with the first entry to read.
    mode
    keyname
    key_expr
    ha_rkey_mode
    cond
    select_limit_cnt
    offset_limit_cnt

  RETURN
    FALSE ok
    TRUE  error
*/
 
bool mysql_ha_read(THD *thd, TABLE_LIST *tables,
                   enum enum_ha_read_modes mode, char *keyname,
                   List<Item> *key_expr,
                   enum ha_rkey_function ha_rkey_mode, Item *cond,
                   ha_rows select_limit_cnt, ha_rows offset_limit_cnt)
{
  TABLE_LIST    *hash_tables;
  TABLE         *table, *backup_open_tables;
  MYSQL_LOCK    *lock;
  List<Item>	list;
  Protocol	*protocol= thd->protocol;
  char		buff[MAX_FIELD_WIDTH];
  String	buffer(buff, sizeof(buff), system_charset_info);
  int           error, keyno= -1;
  uint          num_rows;
  uchar		*UNINIT_VAR(key);
  uint		UNINIT_VAR(key_len);
  bool          need_reopen;
  DBUG_ENTER("mysql_ha_read");
  DBUG_PRINT("enter",("'%s'.'%s' as '%s'",
                      tables->db, tables->table_name, tables->alias));

  if (thd->locked_tables_mode)
  {
    my_error(ER_LOCK_OR_ACTIVE_TRANSACTION, MYF(0));
    DBUG_RETURN(TRUE);
  }

  thd->lex->select_lex.context.resolve_in_table_list_only(tables);
  list.push_front(new Item_field(&thd->lex->select_lex.context,
                                 NULL, NULL, "*"));
  List_iterator<Item> it(list);
  it++;

retry:
  if ((hash_tables= (TABLE_LIST*) my_hash_search(&thd->handler_tables_hash,
                                                 (uchar*) tables->alias,
                                                 strlen(tables->alias) + 1)))
  {
    table= hash_tables->table;
    DBUG_PRINT("info-in-hash",("'%s'.'%s' as '%s' table: 0x%lx",
                               hash_tables->db, hash_tables->table_name,
                               hash_tables->alias, (long) table));
    if (!table)
    {
      /*
        The handler table has been closed. Re-open it.
      */
      if (mysql_ha_open(thd, hash_tables, 1))
      {
        DBUG_PRINT("exit",("reopen failed"));
        goto err0;
      }

      table= hash_tables->table;
      DBUG_PRINT("info",("re-opened '%s'.'%s' as '%s' tab %p",
                         hash_tables->db, hash_tables->table_name,
                         hash_tables->alias, table));
    }
    table->pos_in_table_list= tables;
#if MYSQL_VERSION_ID < 40100
    if (*tables->db && strcmp(table->table_cache_key, tables->db))
    {
      DBUG_PRINT("info",("wrong db"));
      table= NULL;
    }
#endif
  }
  else
    table= NULL;

  if (!table)
  {
#if MYSQL_VERSION_ID < 40100
    char buff[MAX_DBKEY_LENGTH];
    if (*tables->db)
      strxnmov(buff, sizeof(buff)-1, tables->db, ".", tables->table_name,
               NullS);
    else
      strncpy(buff, tables->alias, sizeof(buff));
    my_error(ER_UNKNOWN_TABLE, MYF(0), buff, "HANDLER");
#else
    my_error(ER_UNKNOWN_TABLE, MYF(0), tables->alias, "HANDLER");
#endif
    goto err0;
  }
  tables->table=table;

  /* save open_tables state */
  backup_open_tables= thd->open_tables;
  /*
    mysql_lock_tables() needs thd->open_tables to be set correctly to
    be able to handle aborts properly. When the abort happens, it's
    safe to not protect thd->handler_tables because it won't close any
    tables.
  */
  thd->open_tables= thd->handler_tables;

  lock= mysql_lock_tables(thd, &tables->table, 1, 0, &need_reopen);

  /* restore previous context */
  thd->open_tables= backup_open_tables;

  if (need_reopen)
  {
    mysql_ha_close_table(thd, hash_tables);
    /*
      The lock might have been aborted, we need to manually reset
      thd->some_tables_deleted because handler's tables are closed
      in a non-standard way. Otherwise we might loop indefinitely.
    */
    thd->some_tables_deleted= 0;
    goto retry;
  }

  if (!lock)
    goto err0; // mysql_lock_tables() printed error message already

  // Always read all columns
  tables->table->read_set= &tables->table->s->all_set;

  if (cond)
  {
    if (table->query_id != thd->query_id)
      cond->cleanup();                          // File was reopened
    if ((!cond->fixed &&
	 cond->fix_fields(thd, &cond)) || cond->check_cols(1))
      goto err;
  }

  if (keyname)
  {
    if ((keyno=find_type(keyname, &table->s->keynames, 1+2)-1)<0)
    {
      my_error(ER_KEY_DOES_NOT_EXITS, MYF(0), keyname, tables->alias);
      goto err;
    }
  }

  if (insert_fields(thd, &thd->lex->select_lex.context,
                    tables->db, tables->alias, &it, 0))
    goto err;

  protocol->send_result_set_metadata(&list, Protocol::SEND_NUM_ROWS | Protocol::SEND_EOF);

  /*
    In ::external_lock InnoDB resets the fields which tell it that
    the handle is used in the HANDLER interface. Tell it again that
    we are using it for HANDLER.
  */

  table->file->init_table_handle_for_HANDLER();

  for (num_rows=0; num_rows < select_limit_cnt; )
  {
    switch (mode) {
    case RNEXT:
      if (table->file->inited != handler::NONE)
      {
        error=keyname ?
	  table->file->index_next(table->record[0]) :
	  table->file->rnd_next(table->record[0]);
        break;
      }
      /* else fall through */
    case RFIRST:
      if (keyname)
      {
        table->file->ha_index_or_rnd_end();
        table->file->ha_index_init(keyno, 1);
        error= table->file->index_first(table->record[0]);
      }
      else
      {
        table->file->ha_index_or_rnd_end();
	if (!(error= table->file->ha_rnd_init(1)))
          error= table->file->rnd_next(table->record[0]);
      }
      mode=RNEXT;
      break;
    case RPREV:
      DBUG_ASSERT(keyname != 0);
      if (table->file->inited != handler::NONE)
      {
        error=table->file->index_prev(table->record[0]);
        break;
      }
      /* else fall through */
    case RLAST:
      DBUG_ASSERT(keyname != 0);
      table->file->ha_index_or_rnd_end();
      table->file->ha_index_init(keyno, 1);
      error= table->file->index_last(table->record[0]);
      mode=RPREV;
      break;
    case RNEXT_SAME:
      /* Continue scan on "(keypart1,keypart2,...)=(c1, c2, ...)  */
      DBUG_ASSERT(keyname != 0);
      error= table->file->index_next_same(table->record[0], key, key_len);
      break;
    case RKEY:
    {
      DBUG_ASSERT(keyname != 0);
      KEY *keyinfo=table->key_info+keyno;
      KEY_PART_INFO *key_part=keyinfo->key_part;
      if (key_expr->elements > keyinfo->key_parts)
      {
	my_error(ER_TOO_MANY_KEY_PARTS, MYF(0), keyinfo->key_parts);
	goto err;
      }
      List_iterator<Item> it_ke(*key_expr);
      Item *item;
      key_part_map keypart_map;
      for (keypart_map= key_len=0 ; (item=it_ke++) ; key_part++)
      {
        my_bitmap_map *old_map;
	// 'item' can be changed by fix_fields() call
        if ((!item->fixed &&
             item->fix_fields(thd, it_ke.ref())) ||
	    (item= *it_ke.ref())->check_cols(1))
	  goto err;
	if (item->used_tables() & ~RAND_TABLE_BIT)
        {
          my_error(ER_WRONG_ARGUMENTS,MYF(0),"HANDLER ... READ");
	  goto err;
        }
        old_map= dbug_tmp_use_all_columns(table, table->write_set);
	(void) item->save_in_field(key_part->field, 1);
        dbug_tmp_restore_column_map(table->write_set, old_map);
	key_len+=key_part->store_length;
        keypart_map= (keypart_map << 1) | 1;
      }

      if (!(key= (uchar*) thd->calloc(ALIGN_SIZE(key_len))))
	goto err;
      table->file->ha_index_or_rnd_end();
      table->file->ha_index_init(keyno, 1);
      key_copy(key, table->record[0], table->key_info + keyno, key_len);
      error= table->file->index_read_map(table->record[0],
                                         key, keypart_map, ha_rkey_mode);
      mode=rkey_to_rnext[(int)ha_rkey_mode];
      break;
    }
    default:
      my_message(ER_ILLEGAL_HA, ER(ER_ILLEGAL_HA), MYF(0));
      goto err;
    }

    if (error)
    {
      if (error == HA_ERR_RECORD_DELETED)
        continue;
      if (error != HA_ERR_KEY_NOT_FOUND && error != HA_ERR_END_OF_FILE)
      {
        sql_print_error("mysql_ha_read: Got error %d when reading table '%s'",
                        error, tables->table_name);
        table->file->print_error(error,MYF(0));
        goto err;
      }
      goto ok;
    }
    if (cond && !cond->val_int())
      continue;
    if (num_rows >= offset_limit_cnt)
    {
      protocol->prepare_for_resend();

      if (protocol->send_result_set_row(&list))
        goto err;

      protocol->write();
    }
    num_rows++;
  }
ok:
  mysql_unlock_tables(thd,lock);
  my_eof(thd);
  DBUG_PRINT("exit",("OK"));
  DBUG_RETURN(FALSE);

err:
  mysql_unlock_tables(thd,lock);
err0:
  DBUG_PRINT("exit",("ERROR"));
  DBUG_RETURN(TRUE);
}


/**
  Scan the handler tables hash for matching tables.

  @param thd Thread identifier.
  @param tables The list of tables to remove.

  @return Pointer to head of linked list (TABLE_LIST::next_local) of matching
          TABLE_LIST elements from handler_tables_hash. Otherwise, NULL if no
          table was matched.
*/

static TABLE_LIST *mysql_ha_find(THD *thd, TABLE_LIST *tables)
{
  TABLE_LIST *hash_tables, *head= NULL, *first= tables;
  DBUG_ENTER("mysql_ha_find");

  /* search for all handlers with matching table names */
  for (uint i= 0; i < thd->handler_tables_hash.records; i++)
  {
    hash_tables= (TABLE_LIST*) my_hash_element(&thd->handler_tables_hash, i);
    for (tables= first; tables; tables= tables->next_local)
    {
      if ((! *tables->db ||
          ! my_strcasecmp(&my_charset_latin1, hash_tables->db, tables->db)) &&
          ! my_strcasecmp(&my_charset_latin1, hash_tables->table_name,
                          tables->table_name))
        break;
    }
    if (tables)
    {
      hash_tables->next_local= head;
      head= hash_tables;
    }
  }

  DBUG_RETURN(head);
}


/**
  Remove matching tables from the HANDLER's hash table.

  @param thd Thread identifier.
  @param tables The list of tables to remove.

  @note Broadcasts refresh if it closed a table with old version.
*/

void mysql_ha_rm_tables(THD *thd, TABLE_LIST *tables)
{
  TABLE_LIST *hash_tables, *next;
  DBUG_ENTER("mysql_ha_rm_tables");

  DBUG_ASSERT(tables);

  hash_tables= mysql_ha_find(thd, tables);

  while (hash_tables)
  {
    next= hash_tables->next_local;
    if (hash_tables->table)
      mysql_ha_close_table(thd, hash_tables);
    my_hash_delete(&thd->handler_tables_hash, (uchar*) hash_tables);
    hash_tables= next;
  }

  DBUG_VOID_RETURN;
}


/**
  Flush (close and mark for re-open) all tables that should be should
  be reopen.

  @param thd Thread identifier.

  @note Broadcasts refresh if it closed a table with old version.
*/

void mysql_ha_flush(THD *thd)
{
  TABLE_LIST *hash_tables;
  DBUG_ENTER("mysql_ha_flush");

  safe_mutex_assert_not_owner(&LOCK_open);

  for (uint i= 0; i < thd->handler_tables_hash.records; i++)
  {
    hash_tables= (TABLE_LIST*) my_hash_element(&thd->handler_tables_hash, i);
    /*
      TABLE::mdl_ticket is 0 for temporary tables so we need extra check.
    */
    if (hash_tables->table &&
        (hash_tables->table->mdl_ticket &&
         hash_tables->table->mdl_ticket->has_pending_conflicting_lock() ||
         hash_tables->table->s->needs_reopen()))
      mysql_ha_close_table(thd, hash_tables);
  }

  DBUG_VOID_RETURN;
}


/**
  Close all HANDLER's tables.

  @param thd Thread identifier.

  @note Broadcasts refresh if it closed a table with old version.
*/

void mysql_ha_cleanup(THD *thd)
{
  TABLE_LIST *hash_tables;
  DBUG_ENTER("mysql_ha_cleanup");

  for (uint i= 0; i < thd->handler_tables_hash.records; i++)
  {
    hash_tables= (TABLE_LIST*) my_hash_element(&thd->handler_tables_hash, i);
    if (hash_tables->table)
      mysql_ha_close_table(thd, hash_tables);
  }

  my_hash_free(&thd->handler_tables_hash);

  DBUG_VOID_RETURN;
}

