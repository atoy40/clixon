/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2017 Olof Hagsand and Benny Holmgren

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
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <limits.h>
#include <fnmatch.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <assert.h>
#include <syslog.h>       
#include <fcntl.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include <clixon/clixon.h>

#include "clixon_xmldb_text.h"

#define handle(xh) (assert(text_handle_check(xh)==0),(struct text_handle *)(xh))

/* Magic to ensure plugin sanity. */
#define TEXT_HANDLE_MAGIC 0x7f54da29

/*! Internal structure of text datastore handle. 
 */
struct text_handle {
    int        th_magic;    /* magic */
    char      *th_dbdir;    /* Directory of database files */
    yang_spec *th_yangspec; /* Yang spec if this datastore */
};

/*! Check struct magic number for sanity checks
 * return 0 if OK, -1 if fail.
 */
static int
text_handle_check(xmldb_handle xh)
{
    /* Dont use handle macro to avoid recursion */
    struct text_handle *th = (struct text_handle *)(xh);

    return th->th_magic == TEXT_HANDLE_MAGIC ? 0 : -1;
}

/*! Database locking for candidate and running non-persistent
 * Store an integer for running and candidate containing
 * the session-id of the client holding the lock.
 * @note This should probably be on file-system
 */
static int _running_locked = 0;
static int _candidate_locked = 0;
static int _startup_locked = 0;

/*! Translate from symbolic database name to actual filename in file-system
 * @param[in]   th       text handle handle
 * @param[in]   db       Symbolic database name, eg "candidate", "running"
 * @param[out]  filename Filename. Unallocate after use with free()
 * @retval      0        OK
 * @retval     -1        Error
 * @note Could need a way to extend which databases exists, eg to register new.
 * The currently allowed databases are: 
 *   candidate, tmp, running, result
 * The filename reside in CLICON_XMLDB_DIR option
 */
static int
text_db2file(struct text_handle *th, 
	char               *db,
	char              **filename)
{
    int   retval = -1;
    cbuf *cb;
    char *dir;

    if ((cb = cbuf_new()) == NULL){
	clicon_err(OE_XML, errno, "cbuf_new");
	goto done;
    }
    if ((dir = th->th_dbdir) == NULL){
	clicon_err(OE_XML, errno, "dbdir not set");
	goto done;
    }
    if (strcmp(db, "running") != 0 && 
	strcmp(db, "candidate") != 0 && 
	strcmp(db, "startup") != 0 && 
	strcmp(db, "tmp") != 0){
	clicon_err(OE_XML, 0, "No such database: %s", db);
	goto done;
    }
    cprintf(cb, "%s/%s_db", dir, db);
    if ((*filename = strdup4(cbuf_get(cb))) == NULL){
	clicon_err(OE_UNIX, errno, "strdup");
	goto done;
    }
    retval = 0;
 done:
    if (cb)
	cbuf_free(cb);
    return retval;
}

/*! Connect to a datastore plugin
 * @retval  handle  Use this handle for other API calls
 * @retval  NULL    Error
  */
xmldb_handle
text_connect(void)
{
    struct text_handle *th;
    xmldb_handle        xh = NULL;
    int                 size;

    size = sizeof(struct text_handle);
    if ((th = malloc(size)) == NULL){
	clicon_err(OE_UNIX, errno, "malloc");
	goto done;
    }
    memset(th, 0, size);
    th->th_magic = TEXT_HANDLE_MAGIC;
    xh = (xmldb_handle)th;
  done:
    return xh;

}

/*! Disconnect from to a datastore plugin and deallocate handle
 * @param[in]  xh      XMLDB handle, disconect and deallocate from this handle
 * @retval     0       OK
  */
int
text_disconnect(xmldb_handle xh)
{
    int                 retval = -1;
    struct text_handle *th = handle(xh);

    if (th){
	if (th->th_dbdir)
	    free(th->th_dbdir);
	free(th);
    }
    retval = 0;
    // done:
    return retval;
}

/*! Get value of generic plugin option. Type of value is givenby context
 * @param[in]  xh      XMLDB handle
 * @param[in]  optname Option name
 * @param[out] value   Pointer to Value of option
 * @retval     0       OK
 * @retval    -1       Error
 */
int
text_getopt(xmldb_handle xh, 
	    char        *optname,
	    void       **value)
{
    int               retval = -1;
    struct text_handle *th = handle(xh);

    if (strcmp(optname, "yangspec") == 0)
	*value = th->th_yangspec;
    else if (strcmp(optname, "dbdir") == 0)
	*value = th->th_dbdir;
    else{
	clicon_err(OE_PLUGIN, 0, "Option %s not implemented by plugin", optname);
	goto done;
    }
    retval = 0;
 done:
    return retval;
}

/*! Set value of generic plugin option. Type of value is givenby context
 * @param[in]  xh      XMLDB handle
 * @param[in]  optname Option name
 * @param[in]  value   Value of option
 * @retval     0       OK
 * @retval    -1       Error
 */
int
text_setopt(xmldb_handle xh,
	    char        *optname,
	    void        *value)
{
    int                 retval = -1;
    struct text_handle *th = handle(xh);

    if (strcmp(optname, "yangspec") == 0)
	th->th_yangspec = (yang_spec*)value;
    else if (strcmp(optname, "dbdir") == 0){
	if (value && (th->th_dbdir = strdup((char*)value)) == NULL){
	    clicon_err(OE_UNIX, 0, "strdup");
	    goto done;
	}
    }
    else{
	clicon_err(OE_PLUGIN, 0, "Option %s not implemented by plugin", optname);
	goto done;
    }
    retval = 0;
 done:
    return retval;
}

/*! Populate with spec
 * @param[in]   xt      XML tree with some node marked
 */
int
xml_spec_populate(cxobj  *x, 
		  void   *arg)
{
    int        retval = -1;
    yang_spec *yspec = (yang_spec*)arg;
    char      *name;
    yang_stmt *y;  /* yang node */
    cxobj     *xp; /* xml parent */ 
    yang_stmt *yp; /* parent yang */

    name = xml_name(x);
    if ((xp = xml_parent(x)) != NULL &&
	(yp = xml_spec(xp)) != NULL)
	y = yang_find_syntax((yang_node*)yp, xml_name(x));
    else
	y = yang_find_topnode(yspec, name); /* still NULL for config */
    xml_spec_set(x, y);
    retval = 0;
    // done:
    return retval;
}

/*! Get content of database using xpath. return a set of matching sub-trees
 * The function returns a minimal tree that includes all sub-trees that match
 * xpath.
 * @param[in]  xh     XMLDB handle
 * @param[in]  dbname Name of database to search in (filename including dir path
 * @param[in]  xpath  String with XPATH syntax. or NULL for all
 * @param[out] xtop   Single XML tree which xvec points to. Free with xml_free()
 * @param[out] xvec   Vector of xml trees. Free after use.
 * @param[out] xlen   Length of vector.
 * @retval     0      OK
 * @retval     -1     Error
 * @code
 *   cxobj   *xt;
 *   cxobj  **xvec;
 *   size_t   xlen;
 *   if (xmldb_get(xh, "running", "/interfaces/interface[name="eth"]", 
 *                 &xt, &xvec, &xlen) < 0)
 *      err;
 *   for (i=0; i<xlen; i++){
 *      xn = xv[i];
 *      ...
 *   }
 *   xml_free(xt);
 *   free(xvec);
 * @endcode
 * @note if xvec is given, then purge tree, if not return whole tree.
 * @see xpath_vec
 * @see xmldb_get
 */
int
text_get(xmldb_handle xh,
	 char         *db, 
	 char         *xpath,
	 cxobj       **xtop,
	 cxobj      ***xvec0,
	 size_t       *xlen0)
{
    int             retval = -1;
    char           *dbfile = NULL;
    yang_spec      *yspec;
    cxobj          *xt = NULL;
    int             fd = -1;
    cxobj         **xvec = NULL;
    size_t          xlen;
    int             i;
    struct text_handle *th = handle(xh);

    if (text_db2file(th, db, &dbfile) < 0)
	goto done;
    if (dbfile==NULL){
	clicon_err(OE_XML, 0, "dbfile NULL");
	goto done;
    }
    if ((yspec =  th->th_yangspec) == NULL){
	clicon_err(OE_YANG, ENOENT, "No yang spec");
	goto done;
    }
    if ((fd = open(dbfile, O_RDONLY)) < 0){
	clicon_err(OE_UNIX, errno, "open(%s)", dbfile);
	goto done;
    }    
    /* Parse file into XML tree */
    if ((clicon_xml_parse_file(fd, &xt, "</config>")) < 0)
	goto done;
    /* Always assert a top-level called "config". 
       To ensure that, deal with two cases:
       1. File is empty <top/> -> rename top-level to "config" */
    if (xml_child_nr(xt) == 0){ 
	if (xml_name_set(xt, "config") < 0)
	    goto done;     
    }
    /* 2. File is not empty <top><config>...</config></top> -> replace root */
    else{ 
	assert(xml_child_nr(xt)==1);
	if (xml_rootchild(xt, 0, &xt) < 0)
	    goto done;
    }
    /* XXX Maybe the below is general function and should be moved to xmldb? */
    if (xpath_vec(xt, xpath?xpath:"/", &xvec, &xlen) < 0)
	goto done;

    /* If vectors are specified then filter out everything else,
     * otherwise return complete tree.
     */
    if (xvec != NULL){
	for (i=0; i<xlen; i++)
	    xml_flag_set(xvec[i], XML_FLAG_MARK);
    }
    /* Top is special case */
    if (!xml_flag(xt, XML_FLAG_MARK))
	if (xml_tree_prune_flagged(xt, XML_FLAG_MARK, 1, NULL) < 0)
	    goto done;
    if (xml_apply(xt, CX_ELMNT, (xml_applyfn_t*)xml_flag_reset, (void*)XML_FLAG_MARK) < 0)
	goto done;
    if (xml_apply(xt, CX_ELMNT, xml_spec_populate, yspec) < 0)
	goto done;
    if (xml_apply(xt, CX_ELMNT, xml_default, NULL) < 0)
	goto done;
    /* XXX does not work for top-level */
    if (xml_apply(xt, CX_ELMNT, xml_order, NULL) < 0)
	goto done;
    if (xml_apply(xt, CX_ELMNT, xml_sanity, NULL) < 0)
	goto done;

    if (debug>1)
    	clicon_xml2file(stderr, xt, 0, 1);
    if (xvec0 && xlen0){
	*xvec0 = xvec; 
	xvec = NULL;
	*xlen0 = xlen; 
	xlen = 0;
    }
    *xtop = xt;
    xt = NULL;
    retval = 0;
 done:
    if (xt)
	xml_free(xt);
    if (dbfile)
	free(dbfile);
    if (xvec)
	free(xvec);
    if (fd != -1)
	close(fd);
    return retval;
}

/*! Check if child with fullmatch exists 
 * param[in] cvk vector of index keys 
*/
static cxobj *
find_keys_vec(cxobj *xt, 
	      char  *name, 
	      cvec  *cvk,
	      char **valvec)
{
    cxobj  *xi = NULL;
    int     j;
    char   *keyname;
    char   *val;
    cg_var *cvi;
    char   *body;

    while ((xi = xml_child_each(xt, xi, CX_ELMNT)) != NULL) 
	if (strcmp(xml_name(xi), name) == 0){
	    j = 0; 	    
	    cvi = NULL;
	    while ((cvi = cvec_each(cvk, cvi)) != NULL) {
		keyname = cv_string_get(cvi);
		val = valvec[j++];
		if ((body = xml_find_body(xi, keyname)) == NULL)
		    break;
		if (strcmp(body, val))
		    break;
	    }
	    /* All keys must match: loop terminates. */
	    if (cvi==NULL)
		return xi;
	}
    return NULL;
}

/*! Create 'modification' tree from api-path, ie fill in xml tree from the path
 * @param[in]   api_path  api-path expression
 * @param[in]   xt        XML tree. Find api-path (or create) in this tree
 * @param[in]   op        OP_MERGE, OP_REPLACE, OP_REMOVE, etc 
 * @param[in]   yspec     Yang spec
 * @param[out]  xp        Resulting xml tree corresponding to xt
 * @param[out]  xparp     Parent of xp (xp can be NULL)
 * @param[out]  yp        Yang spec matching xp
 * @see xmldb_put_xkey for example
 */
static int
text_apipath_modify(char               *api_path,
		    cxobj              *xt,
		    enum operation_type op,
		    yang_spec          *yspec,
		    cxobj             **xp,
		    cxobj             **xparp,
		    yang_node         **yp)
{
    int        retval = -1;
    char     **vec = NULL;
    int        nvec;
    int        i;
    int        j;
    char      *name;
    char      *restval;
    yang_stmt *y = NULL;
    yang_stmt *ykey;
    cxobj     *x = NULL;
    cxobj     *xpar = NULL;
    cxobj     *xn = NULL; /* new */
    cxobj     *xb;        /* body */
    cvec      *cvk = NULL; /* vector of index keys */
    char     **valvec = NULL;
    int        nvalvec;
    cg_var    *cvi;
    char      *keyname;
    char      *val2;

    x = xt;
    xpar = xml_parent(xt);
    if (api_path == NULL || *api_path!='/'){
	clicon_err(OE_DB, 0, "Invalid key: %s", api_path);
	goto done;
    }
    if ((vec = clicon_strsep(api_path, "/", &nvec)) == NULL)
	goto done;
    /* Remove trailing '/'. Like in /a/ -> /a */
    if (nvec > 1 && !strlen(vec[nvec-1]))
	nvec--;
    if (nvec < 1){
	clicon_err(OE_XML, 0, "Malformed key: %s", api_path);
	goto done;
    }
    i = 1;
    while (i<nvec){
	name = vec[i]; /* E.g "x=1,2" -> name:x restval=1,2 */
	if ((restval = index(name, '=')) != NULL){
	    *restval = '\0';
	    restval++;
	}
	if (y == NULL) /* top-node */
	    y = yang_find_topnode(yspec, name);
	else 
	    y = yang_find_syntax((yang_node*)y, name);
	if (y == NULL){
	    clicon_err(OE_YANG, errno, "No yang node found: %s", name);
	    goto done;
	}
	i++;
	switch (y->ys_keyword){
	case Y_LEAF_LIST:
	    if (restval==NULL){
		clicon_err(OE_XML, 0, "malformed key, expected '=<restval>'");
		goto done;
	    }
	    /* See if it exists */
	    xn = NULL;
	    while ((xn = xml_child_each(x, xn, CX_ELMNT)) != NULL) 
		if (strcmp(name, xml_name(xn)) == 0 && 
		    strcmp(xml_body(xn),restval)==0)
		    break;
	    if (xn == NULL){ /* Not found, does not exist */
		switch (op){
		case OP_DELETE: /* not here, should be here */
		    clicon_err(OE_XML, 0, "Object to delete does not exist");
		    goto done;
		    break;
		case OP_REMOVE:
		    goto ok; /* not here, no need to remove */
		    break;
		case OP_CREATE:
		    if (i==nvec) /* Last, dont create here */
			break;
		default:
		    //XXX create_keyvalues(cxobj     *x, 
		    if ((xn = xml_new_spec(y->ys_argument, x, y)) == NULL)
			goto done;
		    //		    xml_type_set(xn, CX_ELMNT);
		    if ((xb = xml_new("body", xn)) == NULL)
			goto done; 
		    xml_type_set(xb, CX_BODY);
		    if (xml_value_set(xb, restval) < 0)
			goto done;
		    break;
		}
	    }
	    xpar = x;
	    x = xn;
	    break;
	case Y_LIST:
	    /* Get the yang list key */
	    if ((ykey = yang_find((yang_node*)y, Y_KEY, NULL)) == NULL){
		clicon_err(OE_XML, errno, "%s: List statement \"%s\" has no key", 
			   __FUNCTION__, y->ys_argument);
		goto done;
	    }
	    /* The value is a list of keys: <key>[ <key>]*  */
	    if ((cvk = yang_arg2cvec(ykey, " ")) == NULL)
		goto done;
	    if (restval==NULL){
		clicon_err(OE_XML, 0, "malformed key, expected '=<restval>'");
		goto done;
	    }
	    if (valvec)
		free(valvec);
	    if ((valvec = clicon_strsep(restval, ",", &nvalvec)) == NULL)
		goto done;

	    if (cvec_len(cvk) != nvalvec){ 	    
		clicon_err(OE_XML, errno, "List %s  key length mismatch", name);
		goto done;
	    }
	    cvi = NULL;
	    /* Check if exists, if not, create  */
	    if ((xn = find_keys_vec(x, name, cvk, valvec)) == NULL){
		/* create them, but not if delete op */
		switch (op){
		case OP_DELETE: /* not here, should be here */
		    clicon_err(OE_XML, 0, "Object to delete does not exist");
		    goto done;
		    break;
		case OP_REMOVE:
		    goto ok; /* not here, no need to remove */
		    break;
		default:
		    if ((xn = xml_new(name, x)) == NULL)
			goto done; 
		    xml_type_set(xn, CX_ELMNT);
		    break;
		}
		xpar = x;
		x = xn;
		j = 0;
		while ((cvi = cvec_each(cvk, cvi)) != NULL) {
		    keyname = cv_string_get(cvi);
		    val2 = valvec[j++];
		    if ((xn = xml_new(keyname, x)) == NULL)
			goto done; 
		    xml_type_set(xn, CX_ELMNT);
		    if ((xb = xml_new("body", xn)) == NULL)
			goto done; 
		    xml_type_set(xb, CX_BODY);
		    if (xml_value_set(xb, val2) <0)
			goto done;
		}
	    }
	    else{
		xpar = x;
		x = xn;
	    }
	    if (cvk){
		cvec_free(cvk);
		cvk = NULL;
	    }
	    break;
	default: /* eg Y_CONTAINER, Y_LEAF */
	    if ((xn = xml_find(x, name)) == NULL){
		switch (op){
		case OP_DELETE: /* not here, should be here */
		    clicon_err(OE_XML, 0, "Object to delete does not exist");
		    goto done;
		    break;
		case OP_REMOVE:
		    goto ok; /* not here, no need to remove */
		    break;
		case OP_CREATE:
		    if (i==nvec) /* Last, dont create here */
			break;
		default:
		    if ((xn = xml_new(name, x)) == NULL)
			goto done; 
		    xml_type_set(xn, CX_ELMNT);
		    break;
		}
	    }
	    else{
		if (op==OP_CREATE && i==nvec){ /* here, should not be here */
		    clicon_err(OE_XML, 0, "Object to create already exists");
		    goto done;
		}
	    }
	    xpar = x;
	    x = xn;
	    break;
	}
    }
    *xp = x;
    *xparp = xpar;
    *yp = (yang_node*)y;
 ok:
    retval = 0;
 done:
    if (vec)
	free(vec);
    if (valvec)
	free(valvec);
    return retval;
}

/*! Given a modification tree, check existing matching child in the base tree 
 * param[in] x0  Base tree node
 * param[in] x1c Modification tree child
 * param[in] yc  Yang spec of tree child
*/
static cxobj *
match_base_child(cxobj     *x0, 
		 cxobj     *x1c,
		 yang_stmt *yc)
{
    cxobj     *x0c = NULL;
    char      *keyname;
    cvec      *cvk = NULL;
    cg_var    *cvi;
    char      *b0;
    char      *b1;
    yang_stmt *ykey;
    char      *cname;
    int        ok;
    char      *x1bstr; /* body string */

    cname = xml_name(x1c);
    switch (yc->ys_keyword){
    case Y_LEAF_LIST: /* Match with name and value */
	x1bstr = xml_body(x1c);
	x0c = NULL;
	while ((x0c = xml_child_each(x0, x0c, CX_ELMNT)) != NULL) {
	    if (strcmp(cname, xml_name(x0c)) == 0 && 
		strcmp(xml_body(x0c), x1bstr)==0)
		break;
	}
	break;
    case Y_LIST: /* Match with key values */
	if ((ykey = yang_find((yang_node*)yc, Y_KEY, NULL)) == NULL){
	    clicon_err(OE_XML, errno, "%s: List statement \"%s\" has no key", 
		       __FUNCTION__, yc->ys_argument);
	    goto done;
	}
	/* The value is a list of keys: <key>[ <key>]*  */
	if ((cvk = yang_arg2cvec(ykey, " ")) == NULL)
	    goto done;
	x0c = NULL;
	while ((x0c = xml_child_each(x0, x0c, CX_ELMNT)) != NULL) {
	    if (strcmp(xml_name(x0c), cname))
		continue;
	    cvi = NULL;
	    ok = 0;
	    while ((cvi = cvec_each(cvk, cvi)) != NULL) {
		keyname = cv_string_get(cvi);
		ok = 1; /* if we come here */
		if ((b0 = xml_find_body(x0c, keyname)) == NULL)
		    break; /* error case */
		if ((b1 = xml_find_body(x1c, keyname)) == NULL)
		    break; /* error case */
		if (strcmp(b0, b1))
		    break;
		ok = 2; /* and reaches here for all keynames, x0c is found. */
	    }
	    if (ok == 2)
		break;
	}
	break;
    default: /* Just match with name */
	x0c = xml_find(x0, cname);
	break;
    }
 done:
    if (cvk)
	cvec_free(cvk);
    return x0c;
}

/*! Modify a base tree x0 with x1 with yang spec y according to operation op
 * @param[in]  x0  Base xml tree
 * @param[in]  x1  xml tree which modifies base
 * @param[in]  op  OP_MERGE, OP_REPLACE, OP_REMOVE, etc 
 * @param[in]  y   Yang spec corresponding to xml-node x0. NULL if no x0
 * Assume x0 and x1 are same on entry and that y is the spec
 * @see put in clixon_keyvalue.c
 * XXX: x1 är det som är under x0
 */
static int
text_modify(cxobj              *x0,
	    cxobj              *x0p,
	    cxobj              *x1,
	    enum operation_type op, 
	    yang_node          *y,
	    yang_spec          *yspec)
{
    int        retval = -1;
    char      *opstr;
    char      *name;
    char      *cname; /* child name */
    cxobj     *x0c; /* base child */
    cxobj     *x0b; /* base body */
    cxobj     *x1c; /* mod child */
    char      *x1bstr; /* mod body string */
    yang_stmt *yc;  /* yang child */

    clicon_debug(1, "%s %s", __FUNCTION__, x0?xml_name(x0):"");
    /* Check for operations embedded in tree according to netconf */
    if (x1 && (opstr = xml_find_value(x1, "operation")) != NULL)
	if (xml_operation(opstr, &op) < 0)
	    goto done;
    if (x1 == NULL){
	switch(op){ 
	    case OP_REPLACE:
		if (x0)
		    xml_purge(x0);
	    case OP_CREATE:
	    case OP_MERGE:
		break;
	default:
	    break;
	}
    }
    else {
	assert(xml_type(x1) == CX_ELMNT);
	name = xml_name(x1);
	if (y && (y->yn_keyword == Y_LEAF_LIST || y->yn_keyword == Y_LEAF)){
	    x1bstr = xml_body(x1);
	    switch(op){ 
	    case OP_CREATE:
		if (x0){
		    clicon_err(OE_XML, 0, "Object to create already exists");
		    goto done;
		}
		/* Fall thru */
	    case OP_NONE: /* XXX */
	    case OP_MERGE:
	    case OP_REPLACE:
		if (x0==NULL){
		    if ((x0 = xml_new_spec(name, x0p, y)) == NULL)
			goto done;
		    if (op==OP_NONE)
			xml_flag_set(x0, XML_FLAG_NONE); /* Mark for potential deletion */
		    if (x1bstr){ /* empty type does not have body */
			if ((x0b = xml_new("body", x0)) == NULL)
			    goto done; 
			xml_type_set(x0b, CX_BODY);
		    }
		}
		if (x1bstr){
		    if ((x0b = xml_body_get(x0)) == NULL){
			if ((x0b = xml_new("body", x0)) == NULL)
			    goto done; 
			xml_type_set(x0b, CX_BODY);
		    }
		    if (xml_value_set(x0b, x1bstr) < 0)
			goto done;
		}
		break;
	    case OP_DELETE:
		if (x0==NULL){
		    clicon_err(OE_XML, 0, "Object to delete does not exist");
		    goto done;
		}
	    case OP_REMOVE:
		if (x0)
		    xml_purge(x0);
		break;
	    default:
		break;
	    } /* switch op */
	} /* if LEAF|LEAF_LIST */
	else { /* eg Y_CONTAINER  */
	    switch(op){ 
	    case OP_CREATE:
		/* top-level object <config/> is a special case, ie when 
		 * x0 parent is NULL
		 * or x1 is empty
		 */
		if ((x0p && x0) || 
		    (x0p==NULL && xml_child_nr(x1) == 0)){
		    clicon_err(OE_XML, 0, "Object to create already exists");
		    goto done;
		}
	    case OP_REPLACE:
		/* top-level object <config/> is a special case, ie when 
		 * x0 parent is NULL, 
		 * or x1 is empty
		 */
		if ((x0p && x0) || 
		    (x0p==NULL && xml_child_nr(x1) == 0)){
		    xml_purge(x0);
		    x0 = NULL;
		}
	    case OP_NONE: /* XXX */
	    case OP_MERGE: 
		if (x0==NULL){
		    if ((x0 = xml_new_spec(name, x0p, y)) == NULL)
			goto done;
		    if (op==OP_NONE)
			xml_flag_set(x0, XML_FLAG_NONE); /* Mark for potential deletion */
		}

		/* Loop through children of the modification tree */
		x1c = NULL;
		while ((x1c = xml_child_each(x1, x1c, CX_ELMNT)) != NULL) {
		    cname = xml_name(x1c);
		    /* Get yang spec of the child */
		    if (y == NULL)
			yc = yang_find_topnode(yspec, cname); /* still NULL for config */
		    else{
			if ((yc = yang_find_syntax(y, cname)) == NULL){
			    clicon_err(OE_YANG, errno, "No yang node found: %s", cname);
			    goto done;
			}
		    }
		    /* See if there is a corresponding node in the base tree */
		    x0c = yc?match_base_child(x0, x1c, yc):NULL;
		    if (text_modify(x0c, x0, x1c, op, (yang_node*)yc, yspec) < 0)
			goto done;
		}
		break;
	    case OP_DELETE:
		if (x0==NULL){
		    clicon_err(OE_XML, 0, "Object to delete does not exist");
		    goto done;
		}
	    case OP_REMOVE:
		if (x0)
		    xml_purge(x0);
		break;
	    default:
		break;
	    } /* CONTAINER switch op */
	} /* else Y_CONTAINER  */
    } /* x1 != NULL */
    // ok:
    retval = 0;
 done:
    return retval;
}

/*! Modify database provided an xml tree and an operation
 *
 * @param[in]  xh     XMLDB handle
 * @param[in]  db     running or candidate
 * @param[in]  op     OP_MERGE: just add it. 
 *                    OP_REPLACE: first delete whole database
 *                    OP_NONE: operation attribute in xml determines operation
 * @param[in]  api_path According to restconf (Sec 3.5.1.1 in [restconf-draft 13
])
 * @param[in]  xadd   xml-tree to merge/replace. Top-level symbol is 'config'.
 *                    Should be empty or '<config/>' if delete?
 * @retval     0      OK
 * @retval     -1     Error
 * The xml may contain the "operation" attribute which defines the operation.
 * @code
 *   cxobj     *xt;
 *   if (clicon_xml_parse_str("<a>17</a>", &xt) < 0)
 *     err;
 *   if (xmldb_put(h, "running", OP_MERGE, "/", xt) < 0)
 *     err;
 * @endcode
 */
int
text_put(xmldb_handle        xh,
	 char               *db, 
	 enum operation_type op,
	 char               *api_path,
	 cxobj              *xmod) 
{
    int                 retval = -1;
    struct text_handle *th = handle(xh);
    char               *dbfile = NULL;
    int                 fd = -1;
    cbuf               *cb = NULL;
    cbuf               *xpcb = NULL; /* xpath cbuf */
    yang_spec          *yspec;
    cxobj              *xt = NULL;
    cxobj              *xbase = NULL;
    cxobj              *xbasep = NULL; /* parent */
    cxobj              *xc;
    cxobj              *xnew = NULL;
    yang_node          *y = NULL;

#if 0 /* Just ignore */
    if ((op==OP_DELETE || op==OP_REMOVE) && xmod){
	clicon_err(OE_XML, 0, "xml tree should be NULL for REMOVE/DELETE");
	goto done;
    }
#endif
    if (text_db2file(th, db, &dbfile) < 0)
	goto done;
    if (dbfile==NULL){
	clicon_err(OE_XML, 0, "dbfile NULL");
	goto done;
    }
    if ((yspec =  th->th_yangspec) == NULL){
	clicon_err(OE_YANG, ENOENT, "No yang spec");
	goto done;
    }
    if ((fd = open(dbfile, O_RDONLY)) < 0) {
	clicon_err(OE_UNIX, errno, "open(%s)", dbfile);
	goto done;
    }    
    /* Parse file into XML tree */
    if ((clicon_xml_parse_file(fd, &xt, "</config>")) < 0)
	goto done;
    /* Always assert a top-level called "config". 
       To ensure that, deal with two cases:
       1. File is empty <top/> -> rename top-level to "config" */
    if (xml_child_nr(xt) == 0){ 
	if (xml_name_set(xt, "config") < 0)
	    goto done;     
    }
    /* 2. File is not empty <top><config>...</config></top> -> replace root */
    else{ 
	assert(xml_child_nr(xt)==1);
	if (xml_rootchild(xt, 0, &xt) < 0)
	    goto done;
    }
    /* here xt looks like: <config>...</config> */
    /* If xpath find first occurence or api-path (this is where we apply xml) */
    if (api_path){
	if (text_apipath_modify(api_path, xt, op, yspec, &xbase, &xbasep, &y) < 0)
	    goto done;
    }
    else{
	xbase = xt; /* defer y since x points to config */
	xbasep = xml_parent(xt); /* NULL */
	assert(strcmp(xml_name(xbase),"config")==0);
    }

    /* 
     * Modify base tree x with modification xmod
     */
    if (op == OP_DELETE || op == OP_REMOVE){
	/* special case if top-level, dont purge top-level */
	if (xt == xbase){
	    xc = NULL;
	    while ((xc = xml_child_each(xt, xc, CX_ELMNT)) != NULL){
		xml_purge(xc);
		xc = NULL; /* reset iterator */
	    }
	}
	else
	    if (xbase)
		xml_purge(xbase);
    }
    else 
	if (text_modify(xbase, xbasep, xmod, op, (yang_node*)y, yspec) < 0)
	    goto done;
    /* Remove NONE nodes if all subs recursively are also NONE */
    if (xml_tree_prune_flagged(xt, XML_FLAG_NONE, 0, NULL) <0)
	goto done;
    if (xml_apply(xt, CX_ELMNT, (xml_applyfn_t*)xml_flag_reset, 
		  (void*)XML_FLAG_NONE) < 0)
	goto done;
    // output:
    /* Print out top-level xml tree after modification to file */
    if ((cb = cbuf_new()) == NULL){
	clicon_err(OE_XML, errno, "cbuf_new");
	goto done;
    }
    if (clicon_xml2cbuf(cb, xt, 0, 0) < 0)
	goto done;
    /* Reopen file in write mode */
    close(fd);
    if ((fd = open(dbfile, O_WRONLY | O_TRUNC, S_IRWXU)) < 0) {
	clicon_err(OE_UNIX, errno, "open(%s)", dbfile);
	goto done;
    }    
    if (write(fd, cbuf_get(cb), cbuf_len(cb)+1) < 0){
	clicon_err(OE_UNIX, errno, "write(%s)", dbfile);
	goto done;
    }
    retval = 0;
 done:
    if (dbfile)
	free(dbfile);
    if (fd != -1)
	close(fd);
    if (cb)
	cbuf_free(cb);
    if (xpcb)
	cbuf_free(xpcb);
    if (xt)
	xml_free(xt);
    if (xnew)
	xml_free(xnew);
    return retval;
}

/*! Copy database from db1 to db2
 * @param[in]  xh  XMLDB handle
 * @param[in]  from  Source database copy
 * @param[in]  to    Destination database
 * @retval -1  Error
 * @retval  0  OK
  */
int 
text_copy(xmldb_handle xh, 
	char          *from,
	char          *to)
{
    int                 retval = -1;
    struct text_handle *th = handle(xh);
    char               *fromfile = NULL;
    char               *tofile = NULL;

    /* XXX lock */
    if (text_db2file(th, from, &fromfile) < 0)
	goto done;
    if (text_db2file(th, to, &tofile) < 0)
	goto done;
    if (clicon_file_copy(fromfile, tofile) < 0)
	goto done;
    retval = 0;
 done:
    if (fromfile)
	free(fromfile);
    if (tofile)
	free(tofile);
    return retval;
}

/*! Lock database
 * @param[in]  xh  XMLDB handle
 * @param[in]  db   Database
 * @param[in]  pid  Process id
 * @retval -1  Error
 * @retval  0  OK
  */
int 
text_lock(xmldb_handle xh, 
	char          *db,
	int            pid)
{
    //    struct text_handle *th = handle(xh);

    if (strcmp("running", db) == 0)
	_running_locked = pid;
    else if (strcmp("candidate", db) == 0)
	_candidate_locked = pid;
    else if (strcmp("startup", db) == 0)
	_startup_locked = pid;
    clicon_debug(1, "%s: locked by %u",  db, pid);
    return 0;
}

/*! Unlock database
 * @param[in]  xh  XMLDB handle
 * @param[in]  db  Database
 * @param[in]  pid  Process id
 * @retval -1  Error
 * @retval  0  OK
 * Assume all sanity checks have been made
 */
int 
text_unlock(xmldb_handle xh, 
	    char          *db)
{
    //    struct text_handle *th = handle(xh);

    if (strcmp("running", db) == 0)
	_running_locked = 0;
    else if (strcmp("candidate", db) == 0)
	_candidate_locked = 0;
    else if (strcmp("startup", db) == 0)
	_startup_locked = 0;
    return 0;
}

/*! Unlock all databases locked by pid (eg process dies) 
 * @param[in]    xh  XMLDB handle
 * @param[in]    pid Process / Session id
 * @retval -1    Error
 * @retval   0   Ok
 */
int 
text_unlock_all(xmldb_handle xh, 
	      int            pid)
{
    //    struct text_handle *th = handle(xh);

    if (_running_locked == pid)
	_running_locked = 0;
    if (_candidate_locked == pid)
	_candidate_locked = 0;
    if (_startup_locked == pid)
	_startup_locked = 0;
    return 0;
}

/*! Check if database is locked
 * @param[in]    xh  XMLDB handle
 * @param[in]    db  Database
 * @retval -1    Error
 * @retval   0   Not locked
 * @retval  >0   Id of locker
  */
int 
text_islocked(xmldb_handle xh, 
	    char          *db)
{
    //    struct text_handle *th = handle(xh);

    if (strcmp("running", db) == 0)
	return (_running_locked);
    else if (strcmp("candidate", db) == 0)
	return(_candidate_locked);
    else if (strcmp("startup", db) == 0)
	return(_startup_locked);
    return 0;
}

/*! Check if db exists 
 * @param[in]  xh  XMLDB handle
 * @param[in]  db  Database
 * @retval -1  Error
 * @retval  0  No it does not exist
 * @retval  1  Yes it exists
 */
int 
text_exists(xmldb_handle xh, 
	  char          *db)
{

    int                 retval = -1;
    struct text_handle *th = handle(xh);
    char               *filename = NULL;
    struct stat         sb;

    if (text_db2file(th, db, &filename) < 0)
	goto done;
    if (lstat(filename, &sb) < 0)
	retval = 0;
    else
	retval = 1;
 done:
    if (filename)
	free(filename);
    return retval;
}

/*! Delete database. Remove file 
 * @param[in]  xh  XMLDB handle
 * @param[in]  db  Database
 * @retval -1  Error
 * @retval  0  OK
 */
int 
text_delete(xmldb_handle xh, 
	  char          *db)
{
    int                 retval = -1;
    char               *filename = NULL;
    struct text_handle *th = handle(xh);

    if (text_db2file(th, db, &filename) < 0)
	goto done;
    if (unlink(filename) < 0){
	clicon_err(OE_DB, errno, "unlink %s", filename);
	goto done;
    }
    retval = 0;
 done:
    return retval;
}

/*! Create / init database 
 * @param[in]  xh  XMLDB handle
 * @param[in]  db  Database
 * @retval  0  OK
 * @retval -1  Error
 */
int 
text_create(xmldb_handle xh, 
	  char         *db)
{
    int                 retval = -1;
    struct text_handle *th = handle(xh);
    char               *filename = NULL;
    int                 fd = -1;

    if (text_db2file(th, db, &filename) < 0)
	goto done;
    if ((fd = open(filename, O_CREAT|O_WRONLY, S_IRWXU)) == -1) {
	clicon_err(OE_UNIX, errno, "open(%s)", filename);
	goto done;
    }
   retval = 0;
 done:
    if (filename)
	free(filename);
    if (fd != -1)
	close(fd);
    return retval;
}

/*! plugin exit function */
int
text_plugin_exit(void)
{
    return 0;
}

static const struct xmldb_api api;

/*! plugin init function */
void *
clixon_xmldb_plugin_init(int version)
{
    if (version != XMLDB_API_VERSION){
	clicon_err(OE_DB, 0, "Invalid version %d expected %d", 
		   version, XMLDB_API_VERSION);
	goto done;
    }
    return (void*)&api;
 done:
    return NULL;
}

static const struct xmldb_api api = {
    1,
    XMLDB_API_MAGIC,
    clixon_xmldb_plugin_init,
    text_plugin_exit,
    text_connect,
    text_disconnect,
    text_getopt,
    text_setopt,
    text_get,
    text_put,
    text_copy,
    text_lock,
    text_unlock,
    text_unlock_all,
    text_islocked,
    text_exists,
    text_delete,
    text_create,
};


#if 0 /* Test program */
/*
 * Turn this on to get an xpath test program 
 * Usage: clicon_xpath [<xpath>] 
 * read xml from input
 * Example compile:
 gcc -g -o xmldb -I. -I../clixon ./clixon_xmldb.c -lclixon -lcligen
*/

static int
usage(char *argv0)
{
    fprintf(stderr, "usage:\n%s\tget <db> <yangdir> <yangmod> [<xpath>]\t\txml on stdin\n", argv0);
    fprintf(stderr, "\tput <db> <yangdir> <yangmod> set|merge|delete\txml to stdout\n");
    exit(0);
}

int
main(int argc, char **argv)
{
    cxobj      *xt;
    cxobj      *xn;
    char       *xpath;
    enum operation_type      op;
    char       *cmd;
    char       *db;
    char       *yangdir;
    char       *yangmod;
    yang_spec  *yspec = NULL;
    clicon_handle h;

    if ((h = clicon_handle_init()) == NULL)
	goto done;
    clicon_log_init("xmldb", LOG_DEBUG, CLICON_LOG_STDERR);
    if (argc < 4){
	usage(argv[0]);
	goto done;
    }
    cmd = argv[1];
    db = argv[2];
    yangdir = argv[3];
    yangmod = argv[4];
    db_init(db);
    if ((yspec = yspec_new()) == NULL)
	goto done
    if (yang_parse(h, yangdir, yangmod, NULL, yspec) < 0)
	goto done;
    if (strcmp(cmd, "get")==0){
	if (argc < 5)
	    usage(argv[0]);
	xpath = argc>5?argv[5]:NULL;
	if (xmldb_get(h, db, xpath, &xt, NULL, NULL) < 0)
	    goto done;
	clicon_xml2file(stdout, xt, 0, 1);	
    }
    else
    if (strcmp(cmd, "put")==0){
	if (argc != 6)
	    usage(argv[0]);
	if (clicon_xml_parse_file(0, &xt, "</clicon>") < 0)
	    goto done;
	if (xml_rootchild(xt, 0, &xn) < 0)
	    goto done;
	if (strcmp(argv[5], "set") == 0)
	    op = OP_REPLACE;
	else 	
	    if (strcmp(argv[4], "merge") == 0)
	    op = OP_MERGE;
	else 	if (strcmp(argv[5], "delete") == 0)
	    op = OP_REMOVE;
	else
	    usage(argv[0]);
	if (xmldb_put(h, db, op, NULL, xn) < 0)
	    goto done;
    }
    else
	usage(argv[0]);
    printf("\n");
 done:
    return 0;
}

#endif /* Test program */
