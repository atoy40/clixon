/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2019 Olof Hagsand and Benny Holmgren

  This file is part of CLIXON.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

  Alternatively, the contents of this file may be used under the terms of
  the GNU General Public License Version 3 or later (the "GPL"),
  in which case the provisions of the GPL are applicable instead
  of those above. If you wish to allow use of your version of this file only
  under the terms of the GPL, and not to allow others to
  use your version of this file under the terms of Apache License version 2, 
  indicate your decision by deleting the provisions above and replace them with
  the  notice and other provisions required by the GPL. If you do not delete
  the provisions above, a recipient may use your version of this file under
  the terms of any one of the Apache License version 2 or the GPL.

  ***** END LICENSE BLOCK *****

 *
 * Access functions for clixon data. 
 * Free-typed values for runtime getting and setting.
 *            Accessed with clicon_data(h).
 */

#ifndef _CLIXON_DATA_H_
#define _CLIXON_DATA_H_

/*
 * Constants
 */
/* default group membership to access config unix socket */
#define CLICON_SOCK_GROUP "clicon"

/*
 * Types
 */
/* Struct per database in hash */
typedef struct {
    uint32_t  de_id;  /* session id */
    cxobj    *de_xml; /* cache */
} db_elmnt;

/*
 * Prototypes
 */
yang_stmt * clicon_dbspec_yang(clicon_handle h);
int clicon_dbspec_yang_set(clicon_handle h, yang_stmt *ys);

cvec *clicon_nsctx_global_get(clicon_handle h);
int clicon_nsctx_global_set(clicon_handle h, cvec *nsctx);

cxobj * clicon_nacm_ext(clicon_handle h);
int clicon_nacm_ext_set(clicon_handle h, cxobj *xn);

cxobj *clicon_conf_xml(clicon_handle h);
int clicon_conf_xml_set(clicon_handle h, cxobj *x);

db_elmnt *clicon_db_elmnt_get(clicon_handle h, const char *db);
int clicon_db_elmnt_set(clicon_handle h, const char *db, db_elmnt *xc);

/**/
/* Set and get authorized user name */
char *clicon_username_get(clicon_handle h);
int clicon_username_set(clicon_handle h, void *username);

/* Set and get startup status */
enum startup_status clicon_startup_status_get(clicon_handle h);
int clicon_startup_status_set(clicon_handle h, enum startup_status status);

/* Set and get socket fd (ie backend server socket / restconf fcgx socket */
int clicon_socket_get(clicon_handle h);
int clicon_socket_set(clicon_handle h, int s);

/*! Set and get module state full and brief cached tree */
cxobj *clicon_modst_cache_get(clicon_handle h, int brief);
int clicon_modst_cache_set(clicon_handle h, int brief, cxobj *xms);

/*! Set and get yang/xml module revision changelog */
cxobj *clicon_xml_changelog_get(clicon_handle h);
int clicon_xml_changelog_set(clicon_handle h, cxobj *xchlog);

/*! Set and get user command-line options (after --) */
int clicon_argv_get(clicon_handle h, int *argc, char ***argv);
int clicon_argv_set(clicon_handle h, char *argv0, int argc, char **argv);

int clicon_session_id_set(clicon_handle h, uint32_t id);
uint32_t clicon_session_id_get(clicon_handle h);

#endif  /* _CLIXON_DATA_H_ */
