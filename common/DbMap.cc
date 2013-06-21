// ----------------------------------------------------------------------
// File: DbMap.cc
// Author: Geoffray Adde - CERN
// ----------------------------------------------------------------------

/************************************************************************
 * EOS - the CERN Disk Storage System                                   *
 * Copyright (C) 2011 CERN/Switzerland                                  *
 *                                                                      *
 * This program is free software: you can redistribute it and/or modify *
 * it under the terms of the GNU General Public License as published by *
 * the Free Software Foundation, either version 3 of the License, or    *
 * (at your option) any later version.                                  *
 *                                                                      *
 * This program is distributed in the hope that it will be useful,      *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of       *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the        *
 * GNU General Public License for more details.                         *
 *                                                                      *
 * You should have received a copy of the GNU General Public License    *
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.*
 ************************************************************************/

/*----------------------------------------------------------------------------*/
#include "common/DbMap.hh"
#include "common/Namespace.hh"
#include "XrdSys/XrdSysPthread.hh"
/*----------------------------------------------------------------------------*/
/*----------------------------------------------------------------------------*/
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <fts.h>
#include <iostream>
#include <sstream>
#include <ostream>
#include <istream>
#include <iomanip>
#include <regex.h>
using namespace std;
/*----------------------------------------------------------------------------*/

EOSCOMMONNAMESPACE_BEGIN
/*-------------------- EXPLICIT INSTANTIATIONS -------------------------------*/
template class DbMapT<SqliteDbMapInterface, SqliteDbLogInterface>;
template class DbLogT<SqliteDbMapInterface, SqliteDbLogInterface>;
#ifndef EOS_SQLITE_DBMAP
template class DbMapT<LvDbDbMapInterface, LvDbDbLogInterface>;
template class DbLogT<LvDbDbMapInterface, LvDbDbLogInterface>;
#endif
/*----------------------------------------------------------------------------*/

/*-------------- IMPLEMENTATIONS OF STATIC MEMBER VARIABLES ------------------*/
template<class A, class B> set<string> DbMapT<A, B>::names;
template<class A, class B> eos::common::RWMutex DbMapT<A, B>::namesmutex;
unsigned SqliteInterfaceBase::ninstances = 0;
unsigned SqliteDbLogInterface::ninstances = 0;

RWMutex SqliteInterfaceBase::transactionmutex;
sqlite3 *SqliteInterfaceBase::db = NULL;
bool SqliteInterfaceBase::debugmode = false;
bool SqliteInterfaceBase::abortonsqliteerror = true;
pthread_t SqliteDbLogInterface::archthread;
bool SqliteDbLogInterface::archthreadstarted = false;
XrdSysMutex SqliteDbLogInterface::uniqmutex;
XrdSysCondVar SqliteDbLogInterface::archmutex (0);
SqliteDbLogInterface::tTimeToPeriodedFile SqliteDbLogInterface::archqueue;
SqliteDbLogInterface::tMapFileToSqname SqliteDbLogInterface::file2sqname;
set<int> SqliteDbLogInterface::idpool;

#ifndef EOS_SQLITE_DBMAP
unsigned LvDbInterfaceBase::ninstances = 0;
bool LvDbInterfaceBase::debugmode = false;
bool LvDbInterfaceBase::abortonlvdberror = true;
pthread_t LvDbDbLogInterface::archthread;
bool LvDbDbLogInterface::archthreadstarted = false;
XrdSysMutex LvDbDbLogInterface::uniqmutex;
XrdSysCondVar LvDbDbLogInterface::archmutex (0);
LvDbDbLogInterface::tTimeToPeriodedFile LvDbDbLogInterface::archqueue;
LvDbDbLogInterface::tMapFileToDb LvDbDbLogInterface::file2db;
#endif

/*----------------------------------------------------------------------------*/

void
SqliteInterfaceBase::user_regexp (sqlite3_context *context, int argc, sqlite3_value **argv)
{
  // this function implements the regexp operator for sql3
  // it can be made faster by writing others functions to initialize and terminate the callback
  // so that the compilation of the regex happens only once for a given regex.
#ifdef __APPLE__
  sqlite3_result_error(context, "Regexp not supported on Apple", -1);
  return;
#else
  const char *out;
  const char *pattern;
  const char *input_string;

  struct re_pattern_buffer buffer;
  if ((sqlite3_value_type(argv[0]) != SQLITE_TEXT)
      || (sqlite3_value_type(argv[1]) != SQLITE_TEXT))
  {
    sqlite3_result_error(context, "Improper argument types", -1);
    return;
  }

  re_set_syntax(RE_SYNTAX_POSIX_EGREP);
  memset(&buffer, 0, sizeof (buffer));

  pattern = (const char *) sqlite3_value_text(argv[0]);
  input_string = (const char *) sqlite3_value_text(argv[1]);

  if ((out = re_compile_pattern(pattern, strlen(pattern), &buffer)))
  {
    sqlite3_result_error(context, "Could not compile pattern!", -1);
    regfree(&buffer);
    return;
  }

  if (re_match(&buffer, input_string, strlen(input_string), 0, NULL) < 0)
  {
    regfree(&buffer);
    sqlite3_result_int(context, 0);
  }
  else
  {
    regfree(&buffer);
    sqlite3_result_int(context, 1);
  }
#endif
}

bool operator == (const DbMapTypes::Tlogentry &l, const DbMapTypes::Tlogentry &r)
{
  return !l.timestamp.compare(r.timestamp)
    && !l.timestampstr.compare(r.timestampstr)
    && !l.seqid.compare(r.seqid)
    && !l.writer.compare(r.writer)
    && !l.key.compare(r.key)
    && !l.value.compare(r.value)
    && !l.comment.compare(r.comment);
}

DbMapTypes::Tval
Tlogentry2Tval (const DbMapTypes::Tlogentry &tle)
{
  DbMapTypes::Tval ret;
  ret.timestamp = atol(tle.timestamp.c_str());
  ret.timestampstr = tle.timestampstr;
  ret.seqid = atol(tle.seqid.c_str());
  ret.value = tle.value;
  ret.writer = tle.writer;
  ret.comment = tle.comment;
  return ret;
}

ostream& operator << (ostream &os, const DbMapTypes::Tval &val)
{
  os << setprecision(20) << "\t" << val.timestamp << "\t" << val.timestampstr << "\t" << val.seqid << "\t" << val.value << "\t" << val.comment;
  return os;
}

istream& operator >> (istream &is, DbMapTypes::Tval &val)
{
  string buffer;
  is >> val.timestamp;
  getline(is, val.timestampstr, '\t');
  getline(is, buffer, '\t');
  sscanf(buffer.c_str(), "%lu", &val.seqid);
  getline(is, val.value, '\t');
  getline(is, val.comment, '\t');
  return is;
}

ostream& operator << (ostream &os, const DbMap &map)
{
  const DbMapTypes::Tkey *key;
  const DbMapTypes::Tval *val;
  for (map.BeginIter(); map.Iterate(&key, &val);)
  {
    os << *key << " --> " << *val << endl;
  }
  return os;
}

ostream& operator << (ostream &os, const DbMapTypes::Tlogentry &entry)
{
  os << setprecision(20) << "\ttimestamp=" << entry.timestamp << "\ttimestampstr=" << entry.timestampstr << "\tseqid=" << entry.seqid << "\twriter=" << entry.writer << "\tkey=" << entry.key << "\tvalue=" << entry.value << "\tcomment=" << entry.comment;
  return os;
}

ostream& operator << (ostream &os, const DbMapTypes::TlogentryVec &entryvec)
{
  for (DbMapTypes::TlogentryVec::const_iterator it = entryvec.begin(); it != entryvec.end(); it++)
    os << (*it) << endl;
  return os;
}

#ifndef EOS_SQLITE_DBMAP
typedef DbMapT<SqliteDbMapInterface, SqliteDbLogInterface> DbMapSqlite;
typedef DbMapT<LvDbDbMapInterface, LvDbDbLogInterface> DbMapLeveldb;
typedef DbLogT<SqliteDbLogInterface, SqliteDbLogInterface> DbLogSqlite;
typedef DbLogT<LvDbDbLogInterface, LvDbDbLogInterface> DbLogLeveldb;

DbMapSqlite
DbMapLevelDb2Sqlite (const DbMapLeveldb& toconvert)
{
  DbMapSqlite converted;
  const DbMapLeveldb::Tkey *key = 0;
  const DbMapLeveldb::Tval *val = 0;
  converted.BeginSetSequence();
  toconvert.BeginIter();
  while (toconvert.Iterate(&key, &val))
    converted.Set(*(DbMapSqlite::Tkey*)key, *(DbMapSqlite::Tval*)val);
  converted.EndSetSequence();
  return converted;
}

DbMapLeveldb
DbMapSqlite2LevelDb (const DbMapSqlite& toconvert)
{
  DbMapLeveldb converted;
  const DbMapSqlite::Tkey *key = 0;
  const DbMapSqlite::Tval *val = 0;
  converted.BeginSetSequence();
  toconvert.BeginIter();
  while (toconvert.Iterate(&key, &val))
    converted.Set(*(DbMapLeveldb::Tkey*)key, *(DbMapLeveldb::Tval*)val);
  converted.EndSetSequence();
  return converted;
}

bool
ConvertSqlite2LevelDb (const std::string &sqlpath, const std::string &lvdbpath, const std::string &sqlrename)
{
  const int blocksize = 1e5;
  // if the source and target file have the same name a proper renaming is required for the source
  if ((!sqlpath.compare(lvdbpath)) && (sqlrename.empty() || (!sqlrename.compare(sqlpath))))
    return false;

  // stat the file to copy the permission
  struct stat st;
  if (stat(sqlpath.c_str(), &st)) return false; // cannot stat source file

  if (!sqlrename.empty())
    if (rename(sqlpath.c_str(), sqlrename.c_str())) return false; // cannot rename the source file

  DbLogSqlite sqdbl(sqlrename.empty() ? sqlpath : sqlrename);
  if (!sqdbl.IsOpen())
  { // cannot read the source file
    if (!sqlrename.empty()) rename(sqlrename.c_str(), sqlpath.c_str()); // revert the source renaming
    return false;
  }

  DbMapLeveldb lvdbm;
  if (!lvdbm.AttachLog(lvdbpath, 0, st.st_mode))
  {
    if (!sqlrename.empty()) rename(sqlrename.c_str(), sqlpath.c_str()); // revert the source renaming
    return false; // cannot open the target file
  }

  DbLogSqlite::TlogentryVec sqentryvec;
  DbLogSqlite::Tlogentry sqentry;
  lvdbm.BeginSetSequence();
  while (sqdbl.GetAll(sqentryvec, blocksize, &sqentry))
  {
    for (DbLogSqlite::TlogentryVec::iterator it = sqentryvec.begin(); it != sqentryvec.end(); it++)
      lvdbm.Set(it->key, Tlogentry2Tval(*it)); // keep the original timestamp
    sqentryvec.clear();
  }
  lvdbm.EndSetSequence();
  return true;
}

bool
ConvertLevelDb2Sqlite (const std::string &lvdbpath, const std::string &sqlpath, const std::string &lvdbrename)
{
  const int blocksize = 1e5;
  // if the source and target file have the same name a proper renaming is required for the source
  if ((!lvdbpath.compare(sqlpath)) && (lvdbrename.empty() || (!lvdbrename.compare(lvdbpath))))
    return false;
  // stat the file to copy the permission
  struct stat st;
  if (stat(lvdbpath.c_str(), &st)) return false; // cannot stat source file

  if (!lvdbrename.empty())
    if (rename(lvdbpath.c_str(), lvdbrename.c_str())) return false; // cannot rename the source file

  DbLogLeveldb lvdbl(lvdbrename.empty() ? lvdbpath : lvdbrename);
  if (!lvdbl.IsOpen())
  { // cannot read the source file
    if (!lvdbrename.empty()) rename(lvdbrename.c_str(), sqlpath.c_str()); // revert the source renaming
    return false;
  }

  DbMapSqlite sqdbm;
  if (!sqdbm.AttachLog(sqlpath, 0, st.st_mode & ~0111))
  { // forget the executable mode related to directories
    if (!lvdbrename.empty()) rename(lvdbrename.c_str(), lvdbpath.c_str()); // revert the source renaming
    return false; // cannot open the target file
  }

  DbLogLeveldb::TlogentryVec lventryvec;
  DbLogLeveldb::Tlogentry lventry;
  sqdbm.BeginSetSequence();
  while (lvdbl.GetAll(lventryvec, blocksize, &lventry))
  {
    for (DbLogLeveldb::TlogentryVec::iterator it = lventryvec.begin(); it != lventryvec.end(); it++)
      sqdbm.Set(it->key, Tlogentry2Tval(*it)); // keep the original metadata
    lventryvec.clear();
  }
  sqdbm.EndSetSequence();
  return true;
}
#endif

int
SqliteInterfaceBase::ExecNoCallback (const char* str)
{
  if (debugmode) return Exec(str);
  rc = sqlite3_exec(db, str, NULL, NULL, &errstr);
  TestSqliteError(str, &rc, &errstr, this);
  if (errstr != NULL)
  {
    sqlite3_free(errstr);
    errstr = NULL;
  }
  return rc;
}

int
SqliteInterfaceBase::ExecNoCallback2 (const char* str)
{
  char *errstr;
  int rc;
  if (debugmode) return Exec2(str);
  rc = sqlite3_exec(db, str, NULL, NULL, &errstr);
  TestSqliteError(str, &rc, &errstr, NULL);
  if (errstr != NULL)
  {
    sqlite3_free(errstr);
    errstr = NULL;
  }
  return rc;
}

int
SqliteInterfaceBase::Exec (const char* str)
{
  printf("SQLITE3>> %p executing %s\n", this, str);
  fflush(stdout);
  rc = sqlite3_exec(db, str, PrintCallback, NULL, &errstr);
  printf("SQLITE3>> %p\terror code is %d\n", this, rc);
  printf("SQLITE3>> %p\terror message is %s\n", this, errstr);
  TestSqliteError(str, &rc, &errstr, this);
  if (errstr != NULL)
  {
    sqlite3_free(errstr);
    errstr = NULL;
  }
  return rc;
}

int
SqliteInterfaceBase::Exec2 (const char* str)
{
  char *errstr;
  int rc;
  printf("SQLITE3>> background thread executing %s\n", str);
  fflush(stdout);
  rc = sqlite3_exec(db, str, PrintCallback, NULL, &errstr);
  printf("SQLITE3>> background thread\terror code is %d\n", rc);
  printf("SQLITE3>> background thread\terror message is %s\n", errstr);
  TestSqliteError(str, &rc, &errstr, NULL);
  if (errstr != NULL)
  {
    sqlite3_free(errstr);
    errstr = NULL;
  }
  return rc;
}

int
SqliteInterfaceBase::WrapperCallback (void*isnull, int ncols, char**values, char**keys)
{
  if (ncols != 7) return -1; // given the query there should be 7 columns
  DbMapTypes::TlogentryVec *outputv = this->retvecptr;
  DbMapTypes::Tlogentry newchunk;
  int index = 0;
  newchunk.timestamp = values[index++];
  newchunk.timestampstr = values[index++];
  newchunk.seqid = values[index++];
  newchunk.writer = values[index++];
  newchunk.key = values[index++];
  newchunk.value = values[index++];
  newchunk.comment = values[index++];
  outputv->push_back(newchunk);
  return 0;
}

SqliteDbMapInterface::SqliteDbMapInterface ()
{
  insert_stmt = change_stmt = remove_stmt = NULL;
  if (change_stmt != NULL) sqlite3_finalize(change_stmt);
  if (remove_stmt != NULL) sqlite3_finalize(remove_stmt);
  if (db != NULL)
  {
    sprintf(tablename, "map%p", this);
    char create_statement[1024];
    sprintf(create_statement, "CREATE TABLE %s (timestamp UNSIGNED BIG INT, timestampstr TEXT, seqid INTEGER, key TEXT PRIMARY KEY, value TEXT,comment TEXT);", tablename);
    rc = ExecNoCallback(create_statement);
    PrepareStatements();
  }
}

SqliteDbMapInterface::~SqliteDbMapInterface ()
{
  for (map<string, tOwnedSDLIptr>::iterator it = attacheddbs.begin(); it != attacheddbs.end(); it = attacheddbs.begin())
  { // strange loop because DetachDbLog erase entries from the map
    if (it->second.second)
      DetachDbLog(it->first);
    else
      DetachDbLog(static_cast<DbLogInterface*> (it->second.first));
  }
  if (insert_stmt != NULL) sqlite3_finalize(insert_stmt);
  if (change_stmt != NULL) sqlite3_finalize(change_stmt);
  if (remove_stmt != NULL) sqlite3_finalize(remove_stmt);
}

void
SqliteDbMapInterface::SetName (const string &name)
{
  this->name = name;
  PrepareExportStatement(); // to refresh the writer value in the export statement
}

const string &
SqliteDbMapInterface::GetName () const
{
  return name;
}

int
SqliteDbMapInterface::PrepareStatements ()
{
  const char *dummy;

  if (insert_stmt != NULL) sqlite3_finalize(insert_stmt);
  sprintf(stmt, "INSERT INTO %s VALUES(?,?,?,?,?,?);", tablename);
  rc = sqlite3_prepare_v2(db, stmt, -1, &insert_stmt, &dummy);
  TestSqliteError(stmt, &rc, &errstr, this);

  if (change_stmt != NULL) sqlite3_finalize(change_stmt);
  sprintf(stmt, "UPDATE %s SET timestamp=?, timestampstr=?, seqid=?, value=?, comment=? WHERE key=?;", tablename);
  rc = sqlite3_prepare_v2(db, stmt, -1, &change_stmt, &dummy);
  TestSqliteError(stmt, &rc, &errstr, this);

  if (remove_stmt != NULL) sqlite3_finalize(remove_stmt);
  sprintf(stmt, "DELETE FROM %s WHERE key=?;", tablename);
  rc = sqlite3_prepare_v2(db, stmt, -1, &remove_stmt, &dummy);
  TestSqliteError(stmt, &rc, &errstr, this);

  return SQLITE_OK;
}

int
SqliteDbMapInterface::PrepareExportStatement ()
{
  const char *dummy;
  char buffer[1024];
  for (vector<sqlite3_stmt*>::iterator it = export_stmts.begin(); it != export_stmts.end(); it++) sqlite3_finalize((*it));
  export_stmts.resize(attacheddbs.size());
  int count = 0;
  for (map<string, tOwnedSDLIptr>::iterator it = attacheddbs.begin(); it != attacheddbs.end(); it++)
  {
    sprintf(buffer, "INSERT INTO %s.ondisk (timestamp,timestampstr,seqid,writer,key,value,comment) SELECT timestamp,timestampstr,seqid,?,key,value,comment FROM %s WHERE key=?;",
            it->second.first->GetSqName().c_str(), tablename);
    if (debugmode) printf("SQLITE3>> %p Preparing export statement : %s\n", this, buffer);
    rc = sqlite3_prepare_v2(db, buffer, -1, &export_stmts[count++], &dummy);
    TestSqliteError(buffer, &rc, &errstr, this);
  }
  return true;
}

bool
SqliteDbMapInterface::BeginTransaction ()
{
  transactionmutex.LockWrite();
  return ExecNoCallback("BEGIN TRANSACTION;") == SQLITE_OK;
}

bool
SqliteDbMapInterface::EndTransaction ()
{
  bool rc = (ExecNoCallback("END TRANSACTION;") == SQLITE_OK);
  transactionmutex.UnLockWrite();
  return rc;
}

bool
SqliteDbMapInterface::InsertEntry (const Tkey &key, const Tval &val)
{
#define sqlitecheck  TestSqliteError("Compiled Statement",&rc,&errstr,this);
  const string &writer = val.writer.empty() ? this->name : val.writer;
  rc = sqlite3_bind_int64(insert_stmt, 1, val.timestamp);
  sqlitecheck;
  rc = sqlite3_bind_text(insert_stmt, 2, val.timestampstr.c_str(), -1, NULL);
  sqlitecheck;
  rc = sqlite3_bind_int(insert_stmt, 3, val.seqid);
  sqlitecheck;
  rc = sqlite3_bind_text(insert_stmt, 4, key.c_str(), -1, NULL);
  sqlitecheck;
  rc = sqlite3_bind_text(insert_stmt, 5, val.value.c_str(), -1, NULL);
  sqlitecheck;
  rc = sqlite3_bind_text(insert_stmt, 6, val.comment.c_str(), -1, NULL);
  sqlitecheck;
  rc = sqlite3_step(insert_stmt);
  sqlitecheck;
  rc = sqlite3_reset(insert_stmt);
  sqlitecheck;
  rc = sqlite3_clear_bindings(insert_stmt);
  sqlitecheck;
  if (!attacheddbs.empty())
  {
    for (int i = 0; i < (int) attacheddbs.size(); i++)
    {
      rc = sqlite3_bind_text(export_stmts[i], 1, writer.c_str(), -1, NULL);
      sqlitecheck;
      rc = sqlite3_bind_text(export_stmts[i], 2, key.c_str(), -1, NULL);
      sqlitecheck;
      rc = sqlite3_step(export_stmts[i]);
      sqlitecheck;
      rc = sqlite3_reset(export_stmts[i]);
      sqlitecheck;
      rc = sqlite3_clear_bindings(export_stmts[i]);
      sqlitecheck;
    }
  }
#undef sqlitecheck
  return true;
}

bool
SqliteDbMapInterface::ChangeEntry (const Tkey &key, const Tval &val)
{
#define sqlitecheck  TestSqliteError("Compiled Statement",&rc,&errstr,this);
  const string &writer = val.writer.empty() ? this->name : val.writer;
  rc = sqlite3_bind_int64(change_stmt, 1, val.timestamp);
  sqlitecheck;
  rc = sqlite3_bind_text(change_stmt, 2, val.timestampstr.c_str(), -1, NULL);
  sqlitecheck;
  rc = sqlite3_bind_int(change_stmt, 3, val.seqid);
  sqlitecheck;
  rc = sqlite3_bind_text(change_stmt, 4, val.value.c_str(), -1, NULL);
  sqlitecheck;
  rc = sqlite3_bind_text(change_stmt, 5, val.comment.c_str(), -1, NULL);
  sqlitecheck;
  rc = sqlite3_bind_text(change_stmt, 6, key.c_str(), -1, NULL);
  sqlitecheck;
  rc = sqlite3_step(change_stmt);
  sqlitecheck;
  rc = sqlite3_reset(change_stmt);
  sqlitecheck;
  rc = sqlite3_clear_bindings(change_stmt);
  sqlitecheck;
  if (!attacheddbs.empty())
  {
    for (int i = 0; i < (int) attacheddbs.size(); i++)
    {
      rc = sqlite3_bind_text(export_stmts[i], 1, writer.c_str(), -1, NULL);
      sqlitecheck;
      rc = sqlite3_bind_text(export_stmts[i], 2, key.c_str(), -1, NULL);
      sqlitecheck;
      rc = sqlite3_step(export_stmts[i]);
      sqlitecheck;
      rc = sqlite3_reset(export_stmts[i]);
      sqlitecheck;
      rc = sqlite3_clear_bindings(export_stmts[i]);
      sqlitecheck;
    }
  }
#undef sqlitecheck
  return true;
}

bool
SqliteDbMapInterface::RemoveEntry (const Tkey &key)
{
#define sqlitecheck  TestSqliteError("Compiled Statement",&rc,&errstr,this);
  rc = sqlite3_bind_text(remove_stmt, 1, key.c_str(), -1, NULL);
  sqlitecheck;
  rc = sqlite3_step(remove_stmt);
  sqlitecheck;
  rc = sqlite3_clear_bindings(remove_stmt);
  sqlitecheck;
#undef sqlitecheck
  return true;
}

bool
SqliteDbMapInterface::AttachDbLog (DbLogInterface *dblogint)
{
  string sname = dblogint->GetDbFile();
  if (attacheddbs.find(sname) != attacheddbs.end())
  {
    attacheddbs[sname] = tOwnedSDLIptr(static_cast<SqliteDbLogInterface*> (dblogint), false);
    return PrepareExportStatement();
  }
  return false;
}

bool
SqliteDbMapInterface::DetachDbLog (DbLogInterface *dblogint)
{
  string sname = dblogint->GetDbFile();
  if (attacheddbs.find(sname) != attacheddbs.end())
  {
    // the ownership should be false
    attacheddbs.erase(sname);
    return PrepareExportStatement();
  }
  return false;
}

bool
SqliteDbMapInterface::AttachDbLog (const string &dbname, int sliceduration, int createperm)
{
  if (attacheddbs.find(dbname) == attacheddbs.end())
  {
    attacheddbs[dbname] = tOwnedSDLIptr(new SqliteDbLogInterface(dbname, sliceduration, createperm), true);
    return PrepareExportStatement();
  }
  return false;
}

bool
SqliteDbMapInterface::DetachDbLog (const string &dbname)
{
  if (attacheddbs.find(dbname) != attacheddbs.end())
  {
    delete attacheddbs[dbname].first; // the ownership should be true
    attacheddbs.erase(dbname);
    return PrepareExportStatement();
  }
  return false;
}

SqliteDbLogInterface::SqliteDbLogInterface ()
{
  Init();
  dbname = "";
}

SqliteDbLogInterface::SqliteDbLogInterface (const string &dbname, int sliceduration, int createperm)
{
  Init();
  SetDbFile(dbname, sliceduration, createperm);
}

SqliteDbLogInterface::~SqliteDbLogInterface ()
{
  SetDbFile("", -1, 0);
  uniqmutex.Lock();
  if (file2sqname.empty() && archthreadstarted)
  {
    if (debugmode) printf("Shuting down archiving thread\n");
    XrdSysThread::Cancel(archthread);
    archmutex.Signal(); // wake up the thread to reach the cancel point
    archthreadstarted = false;
    XrdSysThread::Join(archthread, NULL);
  }
  uniqmutex.UnLock();
  AtomicDec(ninstances);
}

void
SqliteDbLogInterface::Init ()
{
  AtomicInc(ninstances);
  isopen = false;
  if (file2sqname.empty()) for (int k = 0; k < 64; k++) idpool.insert(k); // 62 should be enough according to sqlite specs
}

void
SqliteDbLogInterface::ArchiveThreadCleanup (void *dummy)
{
  archmutex.UnLock();
  if (debugmode) printf("Cleaning up archive thread\n");
  fflush(stdout);
}

void*
SqliteDbLogInterface::ArchiveThread (void *dummy)
{
  pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);
  pthread_cleanup_push(SqliteDbLogInterface::ArchiveThreadCleanup, NULL); // this is to make sure the mutex ius unlocked before terminating the thread
  archmutex.Lock();
  while (true)
  {
    timespec now;
    eos::common::Timing::GetTimeSpec(now);

    // process and erase the entry of the queue of which are outdated
    if (archqueue.size() != 0)
      for (tTimeToPeriodedFile::iterator it = archqueue.begin(); it != archqueue.end(); it = archqueue.begin())
      {
        if (now < it->first) break;
        Archive(it);
        UpdateArchiveSchedule(it);
      }
    // sleep untill the next archving has to happen or untill a new archive is added to the queue
    now.tv_sec += 3600; // add 1 hour in case the queue is empty
    int waketime = (int) (archqueue.empty() ? &now : &archqueue.begin()->first)->tv_sec;
    int timedout = archmutex.Wait(waketime - time(0));
    if (timedout) sleep(5); // a timeout to let the db requests started just before the deadline complete, possible cancellation point
  }
  pthread_cleanup_pop(0);
  return NULL;
}

int
SqliteDbLogInterface::Archive (const tTimeToPeriodedFile::iterator &entry)
{
  char timeformat[32];
  char *cptr = timeformat;
  tm t1, t2;
  localtime_r(&entry->first.tv_sec, &t1);
  t2 = t1;
  cptr += sprintf(timeformat, "%%y-%%m-%%d-%%a");
  switch (entry->second.second)
  {
  case testly:
    t1.tm_sec -= 10;
    sprintf(cptr, "_%%Hh%%Mm%%Ss");
    break;
  case hourly:
    t1.tm_hour--;
    sprintf(cptr, "_%%Hh%%Mm%%Ss");
    break;
  case daily:
    t1.tm_mday--;
    break;
  case weekly:
    t1.tm_mday -= 7;
    break;
  default:
    t1.tm_sec -= entry->second.second;
    printf(cptr, "_%%Hh%%Mm%%Ss");
    break;
  }
  time_t tt;
  localtime_r(&(tt = mktime(&t1)), &t1);
  tt = mktime(&t2);
  char dbuf1[256];
  char dbuf2[256];
  strftime(dbuf1, 256, timeformat, &t1);
  strftime(dbuf2, 256, timeformat, &t2);

  const string &filename = entry->second.first;
  char *archivename = new char[filename.size() + 256 * 2 + 4];
  sprintf(archivename, "%s__%s--%s", filename.c_str(), dbuf1, dbuf2);

  char *stmt = new char[1024 + filename.size() + 256 * 2 + 4];
  sprintf(stmt, "ATTACH \'%s\' AS archive;", archivename);
  ExecNoCallback2(stmt);
  sprintf(stmt, "CREATE TABLE IF NOT EXISTS archive.ondisk (timestamp UNSIGNED BIG INT , timestampstr TEXT, seqid INTEGER, writer TEXT, key TEXT, value TEXT,comment TEXT, PRIMARY KEY(timestamp,writer) );");
  ExecNoCallback2(stmt);
  uniqmutex.Lock();
  sprintf(stmt, "INSERT INTO archive.ondisk SELECT * FROM %s.ondisk WHERE timestamp<%ld;", file2sqname[filename].first.c_str(), (tt = mktime(&t2)*1e9));
  uniqmutex.UnLock();
  ExecNoCallback2(stmt);
  sprintf(stmt, "DETACH archive;");
  ExecNoCallback2(stmt);
  uniqmutex.Lock();
  sprintf(stmt, "DELETE FROM %s.ondisk WHERE timestamp<%ld;", file2sqname[filename].first.c_str(), tt);
  uniqmutex.UnLock();
  ExecNoCallback2(stmt);

  printf(" created archive %s\n", archivename);

  delete[] stmt;
  delete[] archivename;

  return 0;
}

int
SqliteDbLogInterface::UpdateArchiveSchedule (const tTimeToPeriodedFile::iterator &entry)
{
  tm t;
  localtime_r(&entry->first.tv_sec, &t);
  switch (entry->second.second)
  {
  case testly:
    t.tm_sec += 10;
    break;
  case hourly:
    t.tm_hour++;
    break;
  case daily:
    t.tm_mday++;
    break;
  case weekly:
    t.tm_mday += 7;
    break;
  default:
    t.tm_sec += entry->second.second;
    break;
  }
  timespec ts;
  ts.tv_sec = mktime(&t);
  ts.tv_nsec = 0;
  tPeriodedFile pf = entry->second;
  /////////pthread_mutex_lock(&archmutex);  // not needed because of pthread_cond_timedwait
  archqueue.erase(entry); // remove the entry of the current archiving
  archqueue.insert(pair<timespec, tPeriodedFile > (ts, pf)); // add a new entry for the next archiving
  archmutex.UnLock();

  return 0;
}

int
SqliteDbLogInterface::SetArchivingPeriod (const string &dbname, int sliceduration)
{
  if (sliceduration > 0)
  {
    archmutex.Lock();
    if (archqueue.empty())
    { // init the archiving thread
      if (debugmode) printf("starting the archive thread\n");
      fflush(stdout);
      XrdSysThread::Run(&archthread, &ArchiveThread, NULL, XRDSYSTHREAD_HOLD, NULL);
      archthreadstarted = true;
    }
    archmutex.UnLock();

    uniqmutex.Lock();
    if (file2sqname.find(dbname) == file2sqname.end())
    {
      uniqmutex.UnLock();
      return 0; // no SqliteDbLogInterface instances related to this dbname
    }
    else
    {
      uniqmutex.UnLock();
      timespec ts;
      eos::common::Timing::GetTimeSpec(ts);
      tm tm;
      localtime_r(&ts.tv_sec, &tm);

      switch (sliceduration)
      {
      case testly:
        tm.tm_sec = ((tm.tm_sec / 10) + 1)*10;
        break;
      case hourly:
        tm.tm_hour++;
        tm.tm_sec = tm.tm_min = 0;
        break;
      case daily:
        tm.tm_mday++;
        tm.tm_sec = tm.tm_min = tm.tm_hour;
        break;
      case weekly:
        tm.tm_mday += (7 - tm.tm_wday);
        tm.tm_sec = tm.tm_min = tm.tm_hour;
        break;
      default:
        tm.tm_sec += sliceduration;
        break;
      }
      ts.tv_sec = mktime(&tm);
      ts.tv_nsec = 0;
      archmutex.Lock();
      tTimeToPeriodedFile::iterator it;
      for (it = archqueue.begin(); it != archqueue.end(); it++)
        if (it->second.first.compare(dbname) == 0)
          break;
      if (it != archqueue.end())
      { // if an entry already exists delete it
        archqueue.erase(it);
      }
      bool awakearchthread = false;
      if (archqueue.empty())
        awakearchthread = true;
      else if (ts < archqueue.begin()->first)
        awakearchthread = true;
      archqueue.insert(pair<timespec, tPeriodedFile > (ts, tPeriodedFile(dbname, sliceduration)));
      archmutex.UnLock();
      if (awakearchthread) archmutex.Signal();

      return file2sqname[dbname].second;
    }
  }
  return -1;
}

bool
SqliteDbLogInterface::SetDbFile (const string &dbname, int sliceduration, int createperm)
{
  // check if the file can be opened, it creates it with the required permissions if it does not exist.
  if (!dbname.empty())
  {
    uniqmutex.Lock();
    if (file2sqname.find(dbname) == file2sqname.end())
    { // we do the check only if the file to attach is not yet attached
      int fd;
      if (createperm > 0)
        fd = open(dbname.c_str(), O_RDWR | O_CREAT, createperm);
      else
        fd = open(dbname.c_str(), O_RDWR | O_CREAT, 0644); // default mask is 644
      if (fd < 0)
      {
        uniqmutex.UnLock();
        return false;
      }
      close(fd);
      // check if the file can be successfully opened by sqlite3
      sprintf(stmt, "ATTACH \'%s\' AS %s_%lu;", dbname.c_str(), "testattach", (unsigned long) pthread_self());
      ExecNoCallback(stmt);
      if (rc != SQLITE_OK)
      {
        uniqmutex.UnLock();
        return false;
      }
      sprintf(stmt, "DETACH %s_%lu;", "testattach", (unsigned long) pthread_self());
      ExecNoCallback(stmt);
    }
    uniqmutex.UnLock();
  }

  char stmt[1024];
  if (!this->dbname.empty())
  { // detach the current sqname
    uniqmutex.Lock();
    tCountedSqname &csqn = file2sqname[this->dbname];
    uniqmutex.UnLock();
    if (csqn.second > 1) csqn.second--; // if there is other instances pointing to the that db DON'T detach
    else
    { // if there is no other instances pointing to the that db DO detach
      archmutex.Lock();
      for (tTimeToPeriodedFile::iterator it = archqueue.begin(); it != archqueue.end(); it++)
      {
        if (it->second.first.compare(this->dbname) == 0)
        { // erase the reference to this db is the archiving queue
          archqueue.erase(it);
          break;
        }
      }
      archmutex.UnLock();
      sprintf(stmt, "DETACH %s;", csqn.first.c_str()); // attached dbs for the logs are
      ExecNoCallback(stmt);
      uniqmutex.Lock();
      file2sqname.erase(this->dbname);
      idpool.insert(atoi(this->sqname.c_str() + 3));
      uniqmutex.UnLock();
    }
    isopen = false;
  }

  this->dbname = dbname;

  if (!dbname.empty())
  {
    uniqmutex.Lock();
    if (file2sqname.find(dbname) != file2sqname.end())
    {
      file2sqname[this->dbname].second++; // if there is already others instances pointing to that db DON'T attach
      sqname = file2sqname[this->dbname].first;
    }
    else
    {
      sprintf(stmt, "log%2.2d", *(idpool.begin())); // take the first id available in the pool
      sqname = stmt;
      idpool.erase(*(idpool.begin())); // remove this id from the pool
      sprintf(stmt, "ATTACH \'%s\' AS %s;", dbname.c_str(), sqname.c_str()); // attached dbs for the logs are
      ExecNoCallback(stmt);
      sprintf(stmt, "CREATE TABLE IF NOT EXISTS %s.ondisk (timestamp UNSIGNED BIG INT, timestampstr TEXT, seqid INTEGER, writer TEXT, key TEXT, value TEXT,comment TEXT, PRIMARY KEY(timestamp,writer) );", sqname.c_str());
      ExecNoCallback(stmt);
      file2sqname[dbname] = tCountedSqname(sqname, 1);
    }
    isopen = true;
    uniqmutex.UnLock();
  }

  if (sliceduration > 0) SetArchivingPeriod(dbname, sliceduration);

  return true;
}

bool
SqliteDbLogInterface::IsOpen () const
{
  return isopen;
}

string
SqliteDbLogInterface::GetDbFile () const
{
  return dbname;
}

size_t
SqliteDbLogInterface::GetByRegex (const string &regex, TlogentryVec &retvec, size_t nmax, Tlogentry *startafter) const
{
  char *stmt = new char[regex.size() + 256];
  size_t count = retvec.size();

  char limit[1024], id[1024];
  limit[0] = id[0] = 0;
  if (nmax) sprintf(limit, " LIMIT %lu", nmax);
  if (startafter) if (startafter->timestamp.length()) sprintf(id, " AND TIMESTAMP>(SELECT TIMESTAMP FROM %s.ondisk WHERE timestamp=%s)", this->GetSqName().c_str(), startafter->timestamp.c_str());
  sprintf(stmt, "SELECT * FROM %s.ondisk WHERE %s%s ORDER BY timestamp%s;", this->GetSqName().c_str(), regex.c_str(), id, limit);
  this->retvecptr = &retvec;
  if (debugmode) printf("SQLITE3>> %p executing %s\n", this, stmt);
  rc = sqlite3_exec(db, stmt, StaticWrapperCallback, (void*) this, &errstr);
  if (debugmode) printf("SQLITE3>> %p \terror code is %d\n", this, rc);
  if (debugmode) printf("SQLITE3>> %p \terror message is %s\n", this, errstr);
  TestSqliteError(stmt, &rc, &errstr, (void*) this);
  if (errstr != NULL)
  {
    sqlite3_free(errstr);
    errstr = NULL;
  }
  delete[] stmt;
  if (startafter)
  {
    if (retvec.empty()) (*startafter) = Tlogentry();
    else (*startafter) = retvec[retvec.size() - 1];
  }
  return retvec.size() - count;
}

size_t
SqliteDbLogInterface::GetTail (int nentries, TlogentryVec &retvec) const
{
  char *stmt = new char[256];
  size_t count = retvec.size();
  sprintf(stmt, "SELECT * FROM (SELECT * FROM %s.ondisk ORDER BY timestamp DESC LIMIT %d) ORDER BY timestamp ASC;", this->GetSqName().c_str(), nentries);
  this->retvecptr = &retvec;
  if (debugmode) printf("SQLITE3>> %p executing %s\n", this, stmt);
  rc = sqlite3_exec(db, stmt, StaticWrapperCallback, (void*) this, &errstr);
  if (debugmode) printf("SQLITE3>> %p \terror code is %d\n", this, rc);
  if (debugmode) printf("SQLITE3>> %p \terror message is %s\n", this, errstr);
  TestSqliteError(stmt, &rc, &errstr, (void*) this);
  if (errstr != NULL)
  {
    sqlite3_free(errstr);
    errstr = NULL;
  }
  delete[] stmt;
  return retvec.size() - count;
}

size_t
SqliteDbLogInterface::GetAll (TlogentryVec &retvec, size_t nmax, Tlogentry *startafter) const
{
  size_t count = retvec.size();
  char limit[1024], id[1024];
  limit[0] = id[0] = 0;
  if (nmax) sprintf(limit, " LIMIT %lu", nmax);
  if (startafter) if (startafter->timestamp.length()) sprintf(id, " WHERE TIMESTAMP>(SELECT TIMESTAMP FROM %s.ondisk WHERE timestamp=%s)", this->GetSqName().c_str(), startafter->timestamp.c_str());
  sprintf(stmt, "SELECT * FROM %s.ondisk%s ORDER BY timestamp%s;", this->GetSqName().c_str(), id, limit);
  this->retvecptr = &retvec;
  if (debugmode) printf("SQLITE3>> %p executing %s\n", this, stmt);
  rc = sqlite3_exec(db, stmt, StaticWrapperCallback, (void*) this, &errstr);
  if (debugmode) printf("SQLITE3>> %p \terror code is %d\n", this, rc);
  if (debugmode) printf("SQLITE3>> %p \terror message is %s\n", this, errstr);
  TestSqliteError(stmt, &rc, &errstr, (void*) this);
  if (errstr != NULL)
  {
    sqlite3_free(errstr);
    errstr = NULL;
  }
  // UPDATE startafter
  if (startafter)
  {
    if (retvec.empty()) (*startafter) = Tlogentry();
    else (*startafter) = retvec[retvec.size() - 1];
  }
  return retvec.size() - count;
}

#ifndef EOS_SQLITE_DBMAP

/*----------------------------------------------------------------------------*/
LvDbDbLogInterface::LvDbDbLogInterface ()
{
  Init();
  dbname = "";
  db = NULL;
}

LvDbDbLogInterface::LvDbDbLogInterface (const string &dbname, int sliceduration, int createperm)
{
  Init();
  SetDbFile(dbname, sliceduration, createperm);
}

LvDbDbLogInterface::~LvDbDbLogInterface ()
{
  SetDbFile("", -1, 0);
  uniqmutex.Lock();
  if (file2db.empty() && archthreadstarted)
  {
    if (debugmode) printf("Shuting down archiving thread\n");
    XrdSysThread::Cancel(archthread);
    archmutex.Signal(); // wake up the thread to reach the cancel point
    archthreadstarted = false;
    XrdSysThread::Join(archthread, NULL);
  }
  uniqmutex.UnLock();
  AtomicDec(ninstances);
}

void
LvDbDbLogInterface::Init ()
{
  isopen = false;
  AtomicInc(ninstances);
}

void
LvDbDbLogInterface::ArchiveThreadCleanup (void *dummy)
{
  archmutex.UnLock();
  if (debugmode) printf("Cleaning up archive thread\n");
  fflush(stdout);
}

void*
LvDbDbLogInterface::ArchiveThread (void *dummy)
{
  pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);
  pthread_cleanup_push(LvDbDbLogInterface::ArchiveThreadCleanup, NULL); // this is to make sure the mutex ius unlocked before terminating the thread
  archmutex.Lock();
  while (true)
  {
    timespec now;
    eos::common::Timing::GetTimeSpec(now);

    // process and erase the entry of the queue of which are outdated
    if (archqueue.size() != 0)
      for (tTimeToPeriodedFile::iterator it = archqueue.begin(); it != archqueue.end(); it = archqueue.begin())
      {
        if (now < it->first) break;
        Archive(it);
        UpdateArchiveSchedule(it);
      }
    // sleep untill the next archving has to happen or untill a new archive is added to the queue
    now.tv_sec += 3600; // add 1 hour in case the queue is empty
    int waketime = (int) (archqueue.empty() ? &now : &archqueue.begin()->first)->tv_sec;
    int timedout = archmutex.Wait(waketime - time(0));
    if (timedout) sleep(5); // a timeout to let the db requests started just before the deadline complete, possible cancellation point
  }
  pthread_cleanup_pop(0);
  return NULL;
}

int
LvDbDbLogInterface::Archive (const tTimeToPeriodedFile::iterator &entry)
{
  char timeformat[32];
  char *cptr = timeformat;
  tm t1, t2;
  localtime_r(&entry->first.tv_sec, &t1);
  t2 = t1;
  cptr += sprintf(timeformat, "%%y-%%m-%%d-%%a");
  switch (entry->second.second)
  {
  case testly:
    t1.tm_sec -= 10;
    sprintf(cptr, "_%%Hh%%Mm%%Ss");
    break;
  case hourly:
    t1.tm_hour--;
    sprintf(cptr, "_%%Hh%%Mm%%Ss");
    break;
  case daily:
    t1.tm_mday--;
    break;
  case weekly:
    t1.tm_mday -= 7;
    break;
  default:
    t1.tm_sec -= entry->second.second;
    printf(cptr, "_%%Hh%%Mm%%Ss");
    break;
  }
  time_t tt;
  localtime_r(&(tt = mktime(&t1)), &t1);
  tt = mktime(&t2)*1e9;
  char dbuf1[256];
  char dbuf2[256];
  strftime(dbuf1, 256, timeformat, &t1);
  strftime(dbuf2, 256, timeformat, &t2);

  const string &filename = entry->second.first;
  char *archivename = new char[filename.size() + 256 * 2 + 4];
  sprintf(archivename, "%s__%s--%s", filename.c_str(), dbuf1, dbuf2);

  leveldb::DB *db = file2db[filename].first.first;
  leveldb::DB *archivedb = NULL;
  leveldb::Options options;
  options.create_if_missing = true;
  leveldb::Status status = leveldb::DB::Open(options, archivename, &archivedb);
  if (debugmode) printf("LEVELDB>> opening db %s --> %p\n", archivename, archivedb);
  TestLvDbError(status, NULL);
  leveldb::WriteBatch batchcp;
  leveldb::WriteBatch batchrm;
  leveldb::Iterator *it = db->NewIterator(leveldb::ReadOptions());
  const int blocksize = 10000;
  int counter = 0;
  for (it->SeekToFirst(); it->Valid(); it->Next())
  {
    time_t t = atol(it->key().ToString().substr(0, 19).c_str());
    if (t < tt)
    {
      batchcp.Put(it->key().ToString(), it->value().ToString());
      batchrm.Delete(it->key());
      if (++counter == blocksize)
      {
        archivedb->Write(leveldb::WriteOptions(), &batchcp);
        batchcp.Clear();
        db->Write(leveldb::WriteOptions(), &batchrm);
        batchrm.Clear();
        counter = 0;
      }
    }
    //else break; // the keys should come in the ascending order so the optimization should be correct, not validated
    // this copy process could be optimized if necessary
  }
  if (counter > 0)
  {
    archivedb->Write(leveldb::WriteOptions(), &batchcp);
    db->Write(leveldb::WriteOptions(), &batchrm);
  }

  delete it;
  if (debugmode) printf("LEVELDB>> closing db --> %p\n", archivedb);
  delete archivedb;
  delete[] archivename;

  return 0;
}

int
LvDbDbLogInterface::UpdateArchiveSchedule (const tTimeToPeriodedFile::iterator &entry)
{
  tm t;
  localtime_r(&entry->first.tv_sec, &t);
  switch (entry->second.second)
  {
  case testly:
    t.tm_sec += 10;
    break;
  case hourly:
    t.tm_hour++;
    break;
  case daily:
    t.tm_mday++;
    break;
  case weekly:
    t.tm_mday += 7;
    break;
  default:
    t.tm_sec += entry->second.second;
    break;
  }
  timespec ts;
  ts.tv_sec = mktime(&t);
  ts.tv_nsec = 0;
  tPeriodedFile pf = entry->second;
  archqueue.erase(entry); // remove the entry of the current archiving
  archqueue.insert(pair<timespec, tPeriodedFile > (ts, pf)); // add a new entry for the next archiving

  return 0;
}

int
LvDbDbLogInterface::SetArchivingPeriod (const string &dbname, int sliceduration)
{
  if (sliceduration > 0)
  {
    archmutex.Lock();
    if (archqueue.empty())
    { // init the archiving thread
      if (debugmode) printf("starting the archive thread\n");
      fflush(stdout);
      XrdSysThread::Run(&archthread, &ArchiveThread, NULL, XRDSYSTHREAD_HOLD, NULL);
      archthreadstarted = true;
    }
    archmutex.UnLock();

    uniqmutex.Lock();
    if (file2db.find(dbname) == file2db.end())
    {
      uniqmutex.UnLock();
      return 0; // no LvDbDbLogInterface instances related to this dbname
    }
    else
    {
      uniqmutex.UnLock();
      timespec ts;
      eos::common::Timing::GetTimeSpec(ts);
      tm tm;
      localtime_r(&ts.tv_sec, &tm);

      switch (sliceduration)
      {
      case testly:
        tm.tm_sec = ((tm.tm_sec / 10) + 1)*10;
        break;
      case hourly:
        tm.tm_hour++;
        tm.tm_sec = tm.tm_min = 0;
        break;
      case daily:
        tm.tm_mday++;
        tm.tm_sec = tm.tm_min = tm.tm_hour;
        break;
      case weekly:
        tm.tm_mday += (7 - tm.tm_wday);
        tm.tm_sec = tm.tm_min = tm.tm_hour;
        break;
      default:
        tm.tm_sec += sliceduration;
        break;
      }
      ts.tv_sec = mktime(&tm);
      ts.tv_nsec = 0;
      archmutex.Lock();
      tTimeToPeriodedFile::iterator it;
      for (it = archqueue.begin(); it != archqueue.end(); it++)
        if (it->second.first.compare(dbname) == 0)
          break;
      if (it != archqueue.end())
      { // if an entry already exists delete it
        archqueue.erase(it);
      }
      bool awakearchthread = false;
      if (archqueue.empty())
        awakearchthread = true;
      else if (ts < archqueue.begin()->first)
        awakearchthread = true;
      archqueue.insert(pair<timespec, tPeriodedFile > (ts, tPeriodedFile(dbname, sliceduration)));
      archmutex.UnLock();
      if (awakearchthread) archmutex.Signal();

      return file2db[dbname].second;
    }
  }
  return -1;
}

bool
LvDbDbLogInterface::SetDbFile (const string &dbname, int sliceduration, int createperm)
{
  // check if the file can be opened, it creates it with the required permissions if it does not exist.
  leveldb::DB *testdb = NULL;
  leveldb::Options options;

  // try to create the directory if it doesn't exist (don't forget to add the x mode to all users to make the directory browsable)
  mkdir(dbname.c_str(), createperm ? createperm | 0111 : 0644 | 0111);

  uniqmutex.Lock();
  archmutex.Lock(); 

  if (!dbname.empty())
  {
    if (file2db.find(dbname) == file2db.end())
    { // we do the check only if the file to attach is not yet attached
      options.create_if_missing = true;
      options.error_if_exists = false;
      options.filter_policy = leveldb::NewBloomFilterPolicy(10);
      leveldb::Status status = leveldb::DB::Open(options, dbname.c_str(), &testdb);
      if (!status.ok())
      {
        delete options.filter_policy;
        archmutex.UnLock();
        uniqmutex.UnLock();            
        return false;
      }
    }
    //uniqmutex.UnLock();
  }

  if (!this->dbname.empty())
  { // detach the current sqname
    tCountedDbAndFilter &csqn = file2db[this->dbname];
    if (csqn.second > 1) csqn.second--; // if there is other instances pointing to the that db DON'T detach
    else
    { // if there is no other instances pointing to the that db DO detach
      for (tTimeToPeriodedFile::iterator it = archqueue.begin(); it != archqueue.end(); it++)
      {
        if (it->second.first.compare(this->dbname) == 0)
        { // erase the reference to this db is the archiving queue
          archqueue.erase(it);
          break;
        }
      }
      if (debugmode) printf("LEVELDB>> closing db --> %p\n", csqn.first.first);
      delete csqn.first.first; // close the DB
      delete csqn.first.second; // delete the filter
      file2db.erase(this->dbname);
      this->db = NULL;
      this->dbname = "";
    }
    isopen = false;
  }

  this->dbname = dbname;

  if (!dbname.empty())
  {
    if (file2db.find(dbname) != file2db.end())
    {
      file2db[this->dbname].second++; // if there is already others instances pointing to that db DON'T attach
      db = file2db[this->dbname].first.first;
    }
    else
    {
      db = testdb;
      file2db[dbname] = tCountedDbAndFilter(DbAndFilter(db, (leveldb::FilterPolicy*)options.filter_policy), 1);
    }
    isopen = true;
  }

  archmutex.UnLock();
  uniqmutex.UnLock();            

  if (sliceduration > 0) SetArchivingPeriod(dbname, sliceduration);

  return true;
}

bool
LvDbDbLogInterface::IsOpen () const
{
  return isopen;
}

string
LvDbDbLogInterface::GetDbFile () const
{
  return dbname;
}

size_t
LvDbDbLogInterface::GetByRegex (const string &sregex, TlogentryVec &retvec, size_t nmax, Tlogentry *startafter) const
{
  size_t count = retvec.size();
  regex_t regex;
  memset(&regex, 0, sizeof (regex));
  int ret;
  const char *pattern;
  string input_string;

  pattern = sregex.c_str();

  if ((ret = regcomp(&regex, pattern, REG_EXTENDED)))
  {
    // eos warning
    eos_warning("Error compiling regex in DbMap : return code %d\n", ret);
    printf("Error compiling regex in DbMap : return code %d\n", ret);
    regfree(&regex);
    return 0;
  }

  leveldb::Iterator *it = db->NewIterator(leveldb::ReadOptions());
  it->SeekToFirst();
  if (startafter)
  {
    if (!startafter->timestamp.empty())
    {
      // if a starting position is given seek to that position
      string skey;
      skey = startafter->timestamp;
      skey.reserve(64); // this size fits for a number of seconds on 9 figures (300 years in the future starting from 1970) and for a precision of 1 nanosecond
      skey += ":";
      skey += startafter->writer;
      it->Seek(skey);
      it->Next();
    }
  }
  if (!nmax) nmax = std::numeric_limits<size_t>::max();
  size_t n = 0;
  for (; it->Valid() && n < nmax; it->Next())
  {
    input_string = it->value().ToString();
    if (!(ret = regexec(&regex, input_string.c_str(), 0, NULL, 0)))
    {
      Tlogentry entry;
      istringstream iss(it->value().ToString());
      entry.timestamp = it->key().ToString();
      int c = 0;
      while (entry.timestamp[c] != ':') c++;
      entry.timestamp.resize(c);
      getline(iss, entry.timestampstr, '\t');
      getline(iss, entry.seqid, '\t');
      getline(iss, entry.writer, '\t');
      getline(iss, entry.key, '\t');
      getline(iss, entry.value, '\t');
      getline(iss, entry.comment, '\t');
      retvec.push_back(entry);
      n++;
    }
  }
  if (startafter)
  {
    if (retvec.empty()) (*startafter) = Tlogentry();
    else (*startafter) = retvec[retvec.size() - 1];
  }
  delete it;
  regfree(&regex);
  return retvec.size() - count;
}

size_t
LvDbDbLogInterface::GetTail (int nentries, TlogentryVec &retvec) const
{
  size_t count = retvec.size();
  leveldb::Iterator *it = db->NewIterator(leveldb::ReadOptions());
  size_t n = 0;
  for (it->SeekToLast(); it->Valid() && (nentries--) > 0; it->Prev())
  {
    Tlogentry entry;
    istringstream iss(it->value().ToString());
    entry.timestamp = it->key().ToString();
    getline(iss, entry.timestampstr, '\t');
    getline(iss, entry.seqid, '\t');
    getline(iss, entry.writer, '\t');
    getline(iss, entry.key, '\t');
    getline(iss, entry.value, '\t');
    getline(iss, entry.comment, '\t');
    retvec.push_back(entry);
    n++;
  }
  std::reverse(&retvec[count], &retvec[count + n - 1]);
  delete it;
  return retvec.size() - count;
}

size_t
LvDbDbLogInterface::GetAll (TlogentryVec &retvec, size_t nmax, Tlogentry *startafter) const
{
  size_t count = retvec.size();
  leveldb::Iterator *it = db->NewIterator(leveldb::ReadOptions());
  it->SeekToFirst();
  if (startafter)
  {
    if (!startafter->timestamp.empty())
    {
      // if a starting position is given seek to that position
      string skey;
      skey = startafter->timestamp;
      skey.reserve(64); // this size fits for a number of seconds on 9 figures (300 years in the future starting from 1970) and for a precision of 1 nanosecond
      skey += ":";
      skey += startafter->writer;
      it->Seek(skey);
      it->Next();
    }
  }
  if (!nmax) nmax = std::numeric_limits<size_t>::max();
  size_t n = 0;
  for (; it->Valid() && n < nmax; it->Next())
  {
    Tlogentry entry;
    istringstream iss(it->value().ToString());
    entry.timestamp = it->key().ToString();
    int c = 0;
    while (entry.timestamp[c] != ':') c++;
    entry.timestamp.resize(c);
    getline(iss, entry.timestampstr, '\t');
    getline(iss, entry.seqid, '\t');
    getline(iss, entry.writer, '\t');
    getline(iss, entry.key, '\t');
    getline(iss, entry.value, '\t');
    getline(iss, entry.comment, '\t');
    retvec.push_back(entry);
    n++;
  }
  if (startafter)
  {
    if (retvec.empty()) (*startafter) = Tlogentry();
    else (*startafter) = retvec[retvec.size() - 1];
  }
  delete it;
  return retvec.size() - count;
}

LvDbDbMapInterface::LvDbDbMapInterface () : batched (false) { }

LvDbDbMapInterface::~LvDbDbMapInterface ()
{
  for (map<string, tOwnedLDLIptr>::iterator it = attacheddbs.begin(); it != attacheddbs.end(); it = attacheddbs.begin())
  { // strange loop because DetachDbLog erase entries from the map
    if (it->second.second)
      DetachDbLog(it->first);
    else
      DetachDbLog(static_cast<DbLogInterface*> (it->second.first));
  }
}

void
LvDbDbMapInterface::SetName (const string &name)
{
  this->name = name;
}

const string &
LvDbDbMapInterface::GetName () const
{
  return name;
}

bool
LvDbDbMapInterface::BeginTransaction ()
{
  batched = true;
  return true;
}

bool
LvDbDbMapInterface::EndTransaction ()
{
  if (batched)
  {
    leveldb::Status status;
    for (map<string, tOwnedLDLIptr>::iterator it = attacheddbs.begin(); it != attacheddbs.end(); it++)
    { // strange loop because DetachDbLog erase entries from the map
      status = it->second.first->db->Write(leveldb::WriteOptions(), &writebatch);
      TestLvDbError(status, this);
    }
    writebatch.Clear();
    batched = false;
  }
  return true;
}

bool
LvDbDbMapInterface::InsertEntry (const Tkey &key, const Tval &val)
{
  string sval;
  sval.reserve(1024);
  string skey;
  skey.reserve(64);
  skey.resize(20, '0'); // this size fits for a number of seconds on 9 figures (300 years in the future starting from 1970) and for a precision of 1 nanosecond
  string tab("\t");
  char sseqid[24];
  modp_ulitoa10(val.timestamp, (char*) (&(skey[0])));
  modp_ulitoa10(val.seqid, sseqid);

  // the key string is timestamp:writer to avoid collision in multithreaded use
  skey[19] = ':';
  skey += val.writer.empty() ? this->name : val.writer;
  ((((((((((sval += val.timestampstr) += tab) += sseqid) += tab) += val.writer.empty() ? this->name : val.writer) += tab) += key) += tab) += val.value) += tab) += val.comment;

  if (batched)
  {
    writebatch.Put(skey, sval);
  }
  else
  {
    for (map<string, tOwnedLDLIptr>::iterator it = attacheddbs.begin(); it != attacheddbs.end(); it++)
    { // strange loop because DetachDbLog erase entries from the map
      leveldb::Status status = it->second.first->db->Put(leveldb::WriteOptions(), skey, sval);
      TestLvDbError(status, this);
    }
  }

  return true;
}

bool
LvDbDbMapInterface::ChangeEntry (const Tkey &key, const Tval &val)
{
  return InsertEntry(key, val);
}

bool
LvDbDbMapInterface::RemoveEntry (const Tkey &key)
{
  if (batched)
  {
    writebatch.Delete(key);
  }
  else
  {
    for (map<string, tOwnedLDLIptr>::iterator it = attacheddbs.begin(); it != attacheddbs.end(); it++)
    { // strange loop because DetachDbLog erase entries from the map
      leveldb::Status status = it->second.first->db->Delete(leveldb::WriteOptions(), key);
      TestLvDbError(status, this);
    }
  }
  return true;
}

bool
LvDbDbMapInterface::AttachDbLog (DbLogInterface *dblogint)
{
  string sname = dblogint->GetDbFile();
  if (attacheddbs.find(sname) != attacheddbs.end())
  {
    attacheddbs[sname] = tOwnedLDLIptr(static_cast<LvDbDbLogInterface*> (dblogint), false);
    return true;
  }
  return false;
}

bool
LvDbDbMapInterface::DetachDbLog (DbLogInterface *dblogint)
{
  string sname = dblogint->GetDbFile();
  if (attacheddbs.find(sname) != attacheddbs.end())
  {
    // the ownership should be false
    attacheddbs.erase(sname);
    return true;
  }
  return false;
}

bool
LvDbDbMapInterface::AttachDbLog (const string &dbname, int sliceduration, int createperm)
{
  if (attacheddbs.find(dbname) == attacheddbs.end())
  {
    attacheddbs[dbname] = tOwnedLDLIptr(new LvDbDbLogInterface(dbname, sliceduration, createperm), true);
    return true;
  }
  return false;
}

bool
LvDbDbMapInterface::DetachDbLog (const string &dbname)
{
  if (attacheddbs.find(dbname) != attacheddbs.end())
  {
    delete attacheddbs[dbname].first; // the ownership should be true
    attacheddbs.erase(dbname);
    return true;
  }
  return false;
}
#endif

EOSCOMMONNAMESPACE_END
