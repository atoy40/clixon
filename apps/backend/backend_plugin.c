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
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <dlfcn.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <netinet/in.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include <clixon/clixon.h>

#include "clixon_backend_transaction.h"
#include "backend_plugin.h"
#include "backend_commit.h"

/*! Request plugins to reset system state
 * The system 'state' should be the same as the contents of running_db
 * @param[in]  h       Clicon handle
 * @param[in]  db      Name of database
 * @retval     0       OK
 * @retval    -1       Error
 */
int
clixon_plugin_reset(clicon_handle h, 
		    char         *db)
{
    clixon_plugin *cp = NULL;
    plgreset_t    *resetfn;          /* Plugin auth */
    int            retval = 1;
    
    while ((cp = clixon_plugin_each(h, cp)) != NULL) {
	if ((resetfn = cp->cp_api.ca_reset) == NULL)
	    continue;
	if ((retval = resetfn(h, db)) < 0) {
	    clicon_debug(1, "plugin_start() failed");
	    return -1;
	}
	break;
    }
    return retval;
}

/*! Go through all backend statedata callbacks and collect state data
 * This is internal system call, plugin is invoked (does not call) this function
 * Backend plugins can register 
 * @param[in]     h       clicon handle
 * @param[in]     yspec   Yang spec
 * @param[in]     xpath   String with XPATH syntax. or NULL for all
 * @param[in,out] xtop    State XML tree is merged with existing tree.
 * @retval       -1       Error
 * @retval        0       Statedata callback failed (xret set with netconf-error)
 * @retval        1       OK
 * @note xtop can be replaced
 */
int
clixon_plugin_statedata(clicon_handle    h,
			yang_stmt       *yspec,
			cvec            *nsc,
			char            *xpath,
			cxobj          **xret)
{
    int             retval = -1;
    int             ret;
    cxobj          *xerr = NULL;
    cxobj          *x = NULL;
    clixon_plugin  *cp = NULL;
    plgstatedata_t *fn;          /* Plugin statedata fn */
    cbuf           *cberr = NULL; 
    
    while ((cp = clixon_plugin_each(h, cp)) != NULL) {
	if ((fn = cp->cp_api.ca_statedata) == NULL)
	    continue;
	if ((x = xml_new("config", NULL, NULL)) == NULL)
	    goto done;
	if (fn(h, nsc, xpath, x) < 0)
	    goto fail;  /* Dont quit here on user callbacks */
	if (xml_apply(x, CX_ELMNT, xml_spec_populate, yspec) < 0)
	    goto done;
	/* Check XML from state callback by validating it. return internal 
	 * error with error cause 
	 */
	if ((ret = xml_yang_validate_all_top(h, x, &xerr)) < 0) 
	    goto done;
	if (ret > 0 && (ret = xml_yang_validate_add(h, x, &xerr)) < 0)
	    goto done;
	if (ret == 0){
	    if ((cberr = cbuf_new()) ==NULL){
		clicon_err(OE_XML, errno, "cbuf_new");
		goto done;
	    }
	    cprintf(cberr, "Internal error: state callback returned invalid XML: ");
	    if (netconf_err2cb(xpath_first(xerr, "rpc-error"), cberr) < 0)
		goto done;
	    if (*xret){
		xml_free(*xret);
		*xret = NULL;
	    }
	    if (netconf_operation_failed_xml(xret, "application", cbuf_get(cberr))< 0)
		goto done;
	    goto fail;
	}
	if ((ret = netconf_trymerge(x, yspec, xret)) < 0)
	    goto done;
	if (ret == 0)
	    goto fail;
	if (x){
	    xml_free(x);
	    x = NULL;
	}
    }
    retval = 1;
 done:
    if (cberr)
	cbuf_free(cberr);
    if (x)
	xml_free(x);
    if (xerr)
	xml_free(xerr);
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Create and initialize transaction */
transaction_data_t *
transaction_new(void)
{
    transaction_data_t *td;
    static uint64_t     id = 0; /* Global transaction id */

    if ((td = malloc(sizeof(*td))) == NULL){
	clicon_err(OE_CFG, errno, "malloc");
	return NULL;
    }
    memset(td, 0, sizeof(*td));
    td->td_id = id++;
    return td;
}

/*! Free transaction structure */
int 
transaction_free(transaction_data_t *td)
{
    if (td->td_src)
	xml_free(td->td_src);
    if (td->td_target)
	xml_free(td->td_target);
    if (td->td_dvec)
	free(td->td_dvec);
    if (td->td_avec)
	free(td->td_avec);
    if (td->td_scvec)
	free(td->td_scvec);
    if (td->td_tcvec)
	free(td->td_tcvec);
    free(td);
    return 0;
}

/* The plugin_transaction routines need access to struct plugin which is local to this file */

/*! Call transaction_begin() in all plugins before a validate/commit.
 * @param[in]  h       Clicon handle
 * @param[in]  td      Transaction data
 * @retval     0       OK
 * @retval    -1       Error: one of the plugin callbacks returned error
 */
int
plugin_transaction_begin(clicon_handle       h, 
			  transaction_data_t *td)
{
    int            retval = 0;
    clixon_plugin *cp = NULL;
    trans_cb_t    *fn;

    while ((cp = clixon_plugin_each(h, cp)) != NULL) {
	if ((fn = cp->cp_api.ca_trans_begin) == NULL)
	    continue;
	if ((retval = fn(h, (transaction_data)td)) < 0){
	    if (!clicon_errno) /* sanity: log if clicon_err() is not called ! */
		clicon_log(LOG_NOTICE, "%s: Plugin '%s' transaction_begin callback does not make clicon_err call on error", 
			       __FUNCTION__, cp->cp_name);

	    break;
	}
    }
    return retval;
}

/*! Call transaction_validate callbacks in all backend plugins
 * @param[in]  h       Clicon handle
 * @param[in]  td      Transaction data
 * @retval     0       OK. Validation succeeded in all plugins
 * @retval    -1       Error: one of the plugin callbacks returned validation fail
 */
int
plugin_transaction_validate(clicon_handle       h, 	 
			    transaction_data_t *td)
{
    int            retval = 0;
    clixon_plugin *cp = NULL;
    trans_cb_t    *fn;

    while ((cp = clixon_plugin_each(h, cp)) != NULL) {
	if ((fn = cp->cp_api.ca_trans_validate) == NULL)
	    continue;
	if ((retval = fn(h, (transaction_data)td)) < 0){
	    if (!clicon_errno) /* sanity: log if clicon_err() is not called ! */
		clicon_log(LOG_NOTICE, "%s: Plugin '%s' transaction_validate callback does not make clicon_err call on error", 
			   __FUNCTION__, cp->cp_name);
	    break;
	}
    }
    return retval;
}

/*! Call transaction_complete() in all plugins after validation (before commit)
 * @param[in]  h       Clicon handle
 * @param[in]  td      Transaction data
 * @retval     0       OK
 * @retval    -1       Error: one of the plugin callbacks returned error
 * @note Call plugins which have commit dependencies?
 * @note Rename to transaction_complete?
 */
int
plugin_transaction_complete(clicon_handle       h, 
			    transaction_data_t *td)
{
    int            retval = 0;
    clixon_plugin *cp = NULL;
    trans_cb_t    *fn;
    
    while ((cp = clixon_plugin_each(h, cp)) != NULL) {
	if ((fn = cp->cp_api.ca_trans_complete) == NULL)
	    continue;
	if ((retval = fn(h, (transaction_data)td)) < 0){
	    if (!clicon_errno) /* sanity: log if clicon_err() is not called ! */
		clicon_log(LOG_NOTICE, "%s: Plugin '%s' trans_complete callback does not make clicon_err call on error", 
			   __FUNCTION__, cp->cp_name);
	    
	    break;
	}
    }
    return retval;
}

/*! Revert a commit
 * @param[in]  h   CLICON handle
 * @param[in]  td  Transaction data
 * @param[in]  nr  The plugin where an error occured. 
 * @retval     0       OK
 * @retval    -1       Error
 * The revert is made in plugin before this one. Eg if error occurred in
 * plugin 2, then the revert will be made in plugins 1 and 0.
 */
int
plugin_transaction_revert(clicon_handle       h, 
			  transaction_data_t *td,
			  int                 nr)
{
    int                retval = 0;
    clixon_plugin     *cp = NULL;
    trans_cb_t        *fn;
    
    while ((cp = clixon_plugin_each_revert(h, cp, nr)) != NULL) {
	if ((fn = cp->cp_api.ca_trans_revert) == NULL)
	    continue;
	if ((retval = fn(h, (transaction_data)td)) < 0){
		clicon_log(LOG_NOTICE, "%s: Plugin '%s' trans_revert callback failed", 
			   __FUNCTION__, cp->cp_name);
		break; 
	}
    }
    return retval; /* ignore errors */
}

/*! Call transaction_commit callbacks in all backend plugins
 * @param[in]  h       Clicon handle
 * @param[in]  td      Transaction data
 * @retval     0       OK
 * @retval    -1       Error: one of the plugin callbacks returned error
 * If any of the commit callbacks fail by returning -1, a revert of the 
 * transaction is tried by calling the commit callbacsk with reverse arguments
 * and in reverse order.
 */
int
plugin_transaction_commit(clicon_handle       h, 
			  transaction_data_t *td)
{
    int            retval = 0;
    clixon_plugin *cp = NULL;
    trans_cb_t    *fn;
    int            i=0;

    while ((cp = clixon_plugin_each(h, cp)) != NULL) {
	i++;
	if ((fn = cp->cp_api.ca_trans_commit) == NULL)
	    continue;
	if ((retval = fn(h, (transaction_data)td)) < 0){
	    if (!clicon_errno) /* sanity: log if clicon_err() is not called ! */
		clicon_log(LOG_NOTICE, "%s: Plugin '%s' trans_commit callback does not make clicon_err call on error", 
			   __FUNCTION__, cp->cp_name);
	    /* Make an effort to revert transaction */
	    plugin_transaction_revert(h, td, i-1); 
	    break;
	}
    }
    return retval;
}

/*! Call transaction_end() in all plugins after a successful commit.
 * @param[in]  h       Clicon handle
 * @param[in]  td      Transaction data
 * @retval     0       OK
 * @retval    -1       Error
 */
int
plugin_transaction_end(clicon_handle h,
		       transaction_data_t *td)
{
    int            retval = 0;
    clixon_plugin *cp = NULL;
    trans_cb_t    *fn;
    
    while ((cp = clixon_plugin_each(h, cp)) != NULL) {
	if ((fn = cp->cp_api.ca_trans_end) == NULL)
	    continue;
	if ((retval = fn(h, (transaction_data)td)) < 0){
	    if (!clicon_errno) /* sanity: log if clicon_err() is not called ! */
		clicon_log(LOG_NOTICE, "%s: Plugin '%s' trans_end callback does not make clicon_err call on error", 
			   __FUNCTION__, cp->cp_name);
	    break;
	}
    }
    return retval;
}

/*! Call transaction_abort() in all plugins after a failed validation/commit.
 * @param[in]  h       Clicon handle
 * @param[in]  td      Transaction data
 * @retval     0       OK
 * @retval    -1       Error
 */
int
plugin_transaction_abort(clicon_handle       h, 
			 transaction_data_t *td)
{
    int            retval = 0;
    clixon_plugin *cp = NULL;
    trans_cb_t    *fn;

    while ((cp = clixon_plugin_each(h, cp)) != NULL) {
	if ((fn = cp->cp_api.ca_trans_abort) == NULL)
	    continue;
	fn(h, (transaction_data)td); /* dont abort on error */
    }
    return retval;
}
	
