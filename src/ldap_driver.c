/*
 * Authors: Martin Nagy <mnagy@redhat.com>
 *          Adam Tkac   <atkac@redhat.com>
 *
 * Copyright (C) 2008, 2009  Red Hat
 * see file 'COPYING' for use and warranty information
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; version 2 or later
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#else
#error "Can't compile without config.h"
#endif

#include <isc/buffer.h>
#include <isc/mem.h>
#include <isc/refcount.h>
#include <isc/util.h>

#include <dns/db.h>
#include <dns/dynamic_db.h>
#include <dns/dbiterator.h>
#include <dns/rdata.h>
#include <dns/rdataclass.h>
#include <dns/rdatalist.h>
#include <dns/rdataset.h>
#include <dns/rdatasetiter.h>
#include <dns/rdatatype.h>
#include <dns/result.h>
#include <dns/types.h>

#include <string.h> /* For memcpy */

#include "compat.h"
#include "ldap_helper.h"
#include "ldap_convert.h"
#include "log.h"
#include "rdlist.h"
#include "util.h"
#include "zone_manager.h"

#ifdef HAVE_VISIBILITY
#define VISIBLE __attribute__((__visibility__("default")))
#else
#define VISIBLE
#endif

#define LDAPDB_MAGIC			ISC_MAGIC('L', 'D', 'P', 'D')
#define VALID_LDAPDB(ldapdb) \
	((ldapdb) != NULL && (ldapdb)->common.impmagic == LDAPDB_MAGIC)

#define LDAPDBNODE_MAGIC		ISC_MAGIC('L', 'D', 'P', 'N')
#define VALID_LDAPDBNODE(ldapdbnode)	ISC_MAGIC_VALID(ldapdbnode, \
							LDAPDBNODE_MAGIC)

static void dbiterator_destroy(dns_dbiterator_t **iteratorp);
static isc_result_t	dbiterator_first(dns_dbiterator_t *iterator);
static isc_result_t	dbiterator_last(dns_dbiterator_t *iterator);
static isc_result_t	dbiterator_seek(dns_dbiterator_t *iterator,
					dns_name_t *name);
static isc_result_t	dbiterator_prev(dns_dbiterator_t *iterator);
static isc_result_t	dbiterator_next(dns_dbiterator_t *iterator);
static isc_result_t	dbiterator_current(dns_dbiterator_t *iterator,
					   dns_dbnode_t **nodep,
					   					   dns_name_t *name);
static isc_result_t	dbiterator_pause(dns_dbiterator_t *iterator);
static isc_result_t	dbiterator_origin(dns_dbiterator_t *iterator,
					  dns_name_t *name);

static dns_dbiteratormethods_t dbiterator_methods = {
	dbiterator_destroy,
	dbiterator_first,
	dbiterator_last,
	dbiterator_seek,
	dbiterator_prev,
	dbiterator_next,
	dbiterator_current,
	dbiterator_pause,
	dbiterator_origin
};

typedef struct {
	dns_dbiterator_t		common;
	ldapdb_node_t			*current;
	ldapdb_nodelist_t		nodelist;
} ldap_dbiterator_t;

static dns_rdatasetmethods_t rdataset_methods;

typedef struct {
	dns_db_t			common;
	isc_refcount_t			refs;
	ldap_instance_t			*ldap_inst;
} ldapdb_t;

typedef struct {
	dns_rdatasetiter_t		common;
	dns_rdatalist_t			*current;
} ldapdb_rdatasetiter_t;

static int dummy;
static void *ldapdb_version = &dummy;

static void free_ldapdb(ldapdb_t *ldapdb);
static void detachnode(dns_db_t *db, dns_dbnode_t **targetp);
static isc_result_t clone_rdatalist_to_rdataset(isc_mem_t *mctx,
						dns_rdatalist_t *rdlist,
						dns_rdataset_t *rdataset);

/* ldapdb_rdatasetiter_t methods */
static void
rdatasetiter_destroy(dns_rdatasetiter_t **iterp)
{
	ldapdb_rdatasetiter_t *ldapdbiter = (ldapdb_rdatasetiter_t *)(*iterp);

	detachnode(ldapdbiter->common.db, &ldapdbiter->common.node);
	SAFE_MEM_PUT_PTR(ldapdbiter->common.db->mctx, ldapdbiter);
	*iterp = NULL;
}

static isc_result_t
rdatasetiter_first(dns_rdatasetiter_t *iter)
{
	ldapdb_rdatasetiter_t *ldapdbiter = (ldapdb_rdatasetiter_t *)iter;
	ldapdb_node_t *node = (ldapdb_node_t *)iter->node;

	if (EMPTY(node->rdatalist))
		return ISC_R_NOMORE;

	ldapdbiter->current = HEAD(node->rdatalist);
	return ISC_R_SUCCESS;
}

static isc_result_t
rdatasetiter_next(dns_rdatasetiter_t *iter)
{
	ldapdb_rdatasetiter_t *ldapdbiter = (ldapdb_rdatasetiter_t *)iter;

	ldapdbiter->current = NEXT(ldapdbiter->current, link);
	return (ldapdbiter->current == NULL) ? ISC_R_NOMORE : ISC_R_SUCCESS;
}

static void
rdatasetiter_current(dns_rdatasetiter_t *iter, dns_rdataset_t *rdataset)
{
	ldapdb_rdatasetiter_t *ldapdbiter = (ldapdb_rdatasetiter_t *)iter;
	isc_result_t result;

	result = clone_rdatalist_to_rdataset(ldapdbiter->common.db->mctx,
					     ldapdbiter->current, rdataset);
	INSIST(result == ISC_R_SUCCESS);
}

static dns_rdatasetitermethods_t rdatasetiter_methods = {
	rdatasetiter_destroy,
	rdatasetiter_first,
	rdatasetiter_next,
	rdatasetiter_current
};

/* ldapdb_node_t functions */
isc_result_t
ldapdbnode_create(isc_mem_t *mctx, dns_name_t *owner, ldapdb_node_t **nodep)
{
	ldapdb_node_t *node = NULL;
	isc_result_t result;

	REQUIRE(nodep != NULL && *nodep == NULL);

	CHECKED_MEM_GET_PTR(mctx, node);
	CHECK(isc_refcount_init(&node->refs, 1));

	dns_name_init(&node->owner, NULL);
	CHECK(dns_name_dup(owner, mctx, &node->owner));

	node->magic = LDAPDBNODE_MAGIC;

	INIT_LIST(node->rdatalist);

	*nodep = node;

	return ISC_R_SUCCESS;

cleanup:
	SAFE_MEM_PUT_PTR(mctx, node);

	return result;
}

/*
 * Clone rdlist and convert it into rdataset.
 */
static isc_result_t
clone_rdatalist_to_rdataset(isc_mem_t *mctx, dns_rdatalist_t *rdlist,
			    dns_rdataset_t *rdataset)
{
	isc_result_t result;
	dns_rdatalist_t *new_rdlist = NULL;

	CHECK(rdatalist_clone(mctx, rdlist, &new_rdlist));

	CHECK(dns_rdatalist_tordataset(new_rdlist, rdataset));
	rdataset->methods = &rdataset_methods;
	isc_mem_attach(mctx, (isc_mem_t **)&rdataset->private5);

	return result;

cleanup:
	if (new_rdlist != NULL) {
		free_rdatalist(mctx, rdlist);
		isc_mem_put(mctx, new_rdlist, sizeof(*new_rdlist));
	}

	return result;
}

/*
 * Our own function for disassociating rdatasets. We will also free the
 * rdatalist that we put inside from clone_rdatalist_to_rdataset.
 */
void
ldapdb_rdataset_disassociate(dns_rdataset_t *rdataset)
{
	dns_rdatalist_t *rdlist;
	isc_mem_t *mctx;

	REQUIRE(rdataset != NULL);

	rdlist = rdataset->private1;
	INSIST(rdlist != NULL);
	mctx = rdataset->private5;

	free_rdatalist(mctx, rdlist);
	SAFE_MEM_PUT_PTR(mctx, rdlist);
	isc_mem_detach(&mctx);
}

void
ldapdb_rdataset_clone(dns_rdataset_t *source, dns_rdataset_t *target)
{
	dns_rdatalist_t *rdlist, *new_rdlist = NULL;
	isc_mem_t *mctx;
	isc_result_t result;

	REQUIRE(source != NULL);

	rdlist = source->private1;
	mctx = source->private5;

	result = rdatalist_clone(mctx, rdlist, &new_rdlist);
	/*
	 * INSIST is bad here but there is no other way how to handle NOMEM
	 * errors
	 */
	INSIST(result == ISC_R_SUCCESS);

	*target = *source;
	target->private1 = new_rdlist;
	target->private2 = NULL; /* Reset iterator */
	target->private5 = NULL;
	isc_mem_attach(mctx, (isc_mem_t **)&target->private5);
}

/*
 * Functions.
 *
 * Most of them don't need db parameter but we are checking if it is valid.
 * Invalid db parameter indicates bug in code.
 */

static void
attach(dns_db_t *source, dns_db_t **targetp)
{
	ldapdb_t *ldapdb = (ldapdb_t *)source;

	REQUIRE(VALID_LDAPDB(ldapdb));

	isc_refcount_increment(&ldapdb->refs, NULL);
	*targetp = source;
}

static void
detach(dns_db_t **dbp)
{
	ldapdb_t *ldapdb = (ldapdb_t *)(*dbp);
	unsigned int refs;

	REQUIRE(VALID_LDAPDB(ldapdb));

	isc_refcount_decrement(&ldapdb->refs, &refs);

	if (refs == 0)
		free_ldapdb(ldapdb);

	*dbp = NULL;
}

static void
free_ldapdb(ldapdb_t *ldapdb)
{
	dns_name_free(&ldapdb->common.origin, ldapdb->common.mctx);
	isc_mem_putanddetach(&ldapdb->common.mctx, ldapdb, sizeof(*ldapdb));
}


/**
 * This method should never be called, because LDAP DB is "persistent".
 * See ispersistent() function.
 */
static isc_result_t
beginload(dns_db_t *db, dns_addrdatasetfunc_t *addp, dns_dbload_t **dbloadp)
{

	UNUSED(db);
	UNUSED(addp);
	UNUSED(dbloadp);

	fatal_error("ldapdb: method beginload() should never be called");

	/* Not reached */
	return ISC_R_SUCCESS;
}

/**
 * This method should never be called, because LDAP DB is "persistent".
 * See ispersistent() function.
 */
static isc_result_t
endload(dns_db_t *db, dns_dbload_t **dbloadp)
{

	UNUSED(db);
	UNUSED(dbloadp);

	fatal_error("ldapdb: method endload() should never be called");

	/* Not reached */
	return ISC_R_SUCCESS;
}

static isc_result_t
dump(dns_db_t *db, dns_dbversion_t *version, const char *filename
#if LIBDNS_VERSION_MAJOR >= 31
     , dns_masterformat_t masterformat
#endif
     )
{

	UNUSED(db);
	UNUSED(version);
	UNUSED(filename);
#if LIBDNS_VERSION_MAJOR >= 31
	UNUSED(masterformat);
#endif

	fatal_error("ldapdb: method dump() should never be called");

	/* Not reached */
	return ISC_R_SUCCESS;
}

static void
currentversion(dns_db_t *db, dns_dbversion_t **versionp)
{
	ldapdb_t *ldapdb = (ldapdb_t *)db;

	REQUIRE(VALID_LDAPDB(ldapdb));
	REQUIRE(versionp != NULL && *versionp == NULL);

	*versionp = ldapdb_version;
}

static isc_result_t
newversion(dns_db_t *db, dns_dbversion_t **versionp)
{
	ldapdb_t *ldapdb = (ldapdb_t *)db;

	REQUIRE(VALID_LDAPDB(ldapdb));
	REQUIRE(versionp != NULL && *versionp == NULL);

	*versionp = ldapdb_version;
	return ISC_R_SUCCESS;
}

static void
attachversion(dns_db_t *db, dns_dbversion_t *source,
	      dns_dbversion_t **targetp)
{
	ldapdb_t *ldapdb = (ldapdb_t *)db;

	REQUIRE(VALID_LDAPDB(ldapdb));
	REQUIRE(source == ldapdb_version);
	REQUIRE(targetp != NULL && *targetp == NULL);

	*targetp = ldapdb_version;
}

static void
closeversion(dns_db_t *db, dns_dbversion_t **versionp, isc_boolean_t commit)
{
	ldapdb_t *ldapdb = (ldapdb_t *)db;

	UNUSED(commit);

	REQUIRE(VALID_LDAPDB(ldapdb));
	REQUIRE(versionp != NULL && *versionp == ldapdb_version);

	*versionp = NULL;
}

static isc_result_t
findnode(dns_db_t *db, dns_name_t *name, isc_boolean_t create,
	 dns_dbnode_t **nodep)
{
	ldapdb_t *ldapdb = (ldapdb_t *) db;
	isc_result_t result;
	ldapdb_rdatalist_t rdatalist;
	ldapdb_node_t *node = NULL;

	REQUIRE(VALID_LDAPDB(ldapdb));

	result = ldapdb_rdatalist_get(ldapdb->common.mctx, ldapdb->ldap_inst,
				      name, &ldapdb->common.origin,
				      &rdatalist);

	if (create == ISC_FALSE) {
		if (result != ISC_R_SUCCESS)
			goto cleanup;
	} else {
		if (result != ISC_R_NOTFOUND && result != ISC_R_SUCCESS)
			goto cleanup;
	}

	CHECK(ldapdbnode_create(ldapdb->common.mctx, name, &node));
	memcpy(&node->rdatalist, &rdatalist, sizeof(rdatalist));

	*nodep = node;

	return ISC_R_SUCCESS;

cleanup:
	ldapdb_rdatalist_destroy(ldapdb->common.mctx, &rdatalist);

	return result;
}

/* XXX add support for DNAME redirection */
static isc_result_t
find(dns_db_t *db, dns_name_t *name, dns_dbversion_t *version,
     dns_rdatatype_t type, unsigned int options, isc_stdtime_t now,
     dns_dbnode_t **nodep, dns_name_t *foundname, dns_rdataset_t *rdataset,
     dns_rdataset_t *sigrdataset)
{
	ldapdb_t *ldapdb = (ldapdb_t *) db;
	isc_result_t result = ISC_R_FAILURE;
	ldapdb_node_t *node = NULL;
	dns_rdatalist_t *rdlist = NULL;
	isc_boolean_t is_cname = ISC_FALSE;
	isc_boolean_t is_delegation = ISC_FALSE;
	ldapdb_rdatalist_t rdatalist;
	unsigned int labels, qlabels;
	dns_fixedname_t fname;
	dns_name_t *traversename;

	UNUSED(now);
	UNUSED(sigrdataset);

	REQUIRE(VALID_LDAPDB(ldapdb));
	REQUIRE(!(node != NULL && type == dns_rdatatype_any));
	//REQUIRE(!(node == NULL && rdataset != NULL));

	if (version != NULL) {
		REQUIRE(version == ldapdb_version);
	}

	if (!dns_name_issubdomain(name, &db->origin))
		return DNS_R_NXDOMAIN;

	labels = dns_name_countlabels(&db->origin);
	qlabels = dns_name_countlabels(name);

	/*
	 * We need to find the best match in LDAP. We can receive question for
	 * any name beneath db->origin. We perform "up->down" traverse and
	 * when we hit delegation point, we stop and return an answer.
	 * If we don't find any delegation, during the path, we work with the
	 * full match and return answer or NXDOMAIN.
	 */
	dns_fixedname_init(&fname);
	traversename = dns_fixedname_name(&fname);

	for (; labels <= qlabels; labels++) {
		dns_name_getlabelsequence(name, qlabels - labels, labels,
					  traversename);

		result = ldapdb_rdatalist_get(ldapdb->common.mctx,
					      ldapdb->ldap_inst, traversename,
					      &ldapdb->common.origin,
					      &rdatalist);
		if (result != ISC_R_SUCCESS) {
			result = DNS_R_NXDOMAIN;
			continue;
		}

		/* TODO: We should check for DNAME records right here */

		/*
		 * Check if there is at least one NS RR. If yes and this is not NS
		 * record of this zone (i.e. NS record has higher number of labels),
		 * we hit delegation point. Delegation point has the highest priority
		 * and we supress all other RR types than NS, except when we are
		 * trying to find glue.
		 */
		if (dns_name_countlabels(&db->origin) <
		    dns_name_countlabels(traversename) &&
		    (options & DNS_DBFIND_GLUEOK) == 0) {
			result = ldapdb_rdatalist_findrdatatype(&rdatalist,
								dns_rdatatype_ns,
								&rdlist);
			if (result == ISC_R_SUCCESS) {
				/* Delegation point */
				type = dns_rdatatype_ns;
				is_delegation = ISC_TRUE;
				goto skipfind;
			}
		}

		if (labels == qlabels) {
			/* We've found an answer */
			goto found;
		} else {
			ldapdb_rdatalist_destroy(ldapdb->common.mctx,
						 &rdatalist);
		}
	}

	if (result != ISC_R_SUCCESS)
		return (result == ISC_R_NOTFOUND) ? DNS_R_NXDOMAIN : result;

found:
	/*
	 * ANY pseudotype indicates the whole node, skip routines
	 * which attempts to find the exact RR type.
	 */
	if (type == dns_rdatatype_any)
		goto skipfind;

	result = ldapdb_rdatalist_findrdatatype(&rdatalist, type, &rdlist);

	if (result != ISC_R_SUCCESS) {
		/* No exact rdtype match. Check CNAME */

		rdlist = HEAD(rdatalist);
		while (rdlist != NULL && rdlist->type != dns_rdatatype_cname)
			rdlist = NEXT(rdlist, link);

		/* CNAME was found */
		if (rdlist != NULL) {
			result = ISC_R_SUCCESS;
			is_cname = ISC_TRUE;
		}
	}

	if (result != ISC_R_SUCCESS) {
		result = DNS_R_NXRRSET;
		goto cleanup;
	}

skipfind:
	CHECK(dns_name_copy(traversename, foundname, NULL));

	if (rdataset != NULL && type != dns_rdatatype_any) {
		/* dns_rdatalist_tordataset returns success only */
		CHECK(clone_rdatalist_to_rdataset(ldapdb->common.mctx, rdlist,
						  rdataset));
	}

	if (nodep != NULL) {
		CHECK(ldapdbnode_create(ldapdb->common.mctx, traversename, &node));
		memcpy(&node->rdatalist, &rdatalist, sizeof(rdatalist));
		*nodep = node;
	} else {
		ldapdb_rdatalist_destroy(ldapdb->common.mctx, &rdatalist);
	}

	if (is_delegation)
		return DNS_R_DELEGATION;
	else if (is_cname)
		return DNS_R_CNAME;
	else
		return ISC_R_SUCCESS;

cleanup:
	ldapdb_rdatalist_destroy(ldapdb->common.mctx, &rdatalist);
	return result;
}

static isc_result_t
findzonecut(dns_db_t *db, dns_name_t *name, unsigned int options,
	    isc_stdtime_t now, dns_dbnode_t **nodep, dns_name_t *foundname,
	    dns_rdataset_t *rdataset, dns_rdataset_t *sigrdataset)
{
	UNUSED(db);
	UNUSED(name);
	UNUSED(options);
	UNUSED(now);
	UNUSED(nodep);
	UNUSED(foundname);
	UNUSED(rdataset);
	UNUSED(sigrdataset);

	return ISC_R_NOTIMPLEMENTED;
}

static void
attachnode(dns_db_t *db, dns_dbnode_t *source, dns_dbnode_t **targetp)
{
	ldapdb_node_t *node = (ldapdb_node_t *) source;

	REQUIRE(VALID_LDAPDBNODE(node));

	UNUSED(db);

	isc_refcount_increment(&node->refs, NULL);
	*targetp = source;
}

static void
detachnode(dns_db_t *db, dns_dbnode_t **targetp)
{
	ldapdb_node_t *node = (ldapdb_node_t *)(*targetp);
	ldapdb_t *ldapdb = (ldapdb_t *) db;
	unsigned int refs;

	/*
	 * Don't check for db and targetp validity, it's done in
	 * dns_db_detachnode
	 */

	REQUIRE(VALID_LDAPDBNODE(node));
	isc_refcount_decrement(&node->refs, &refs);
	if (refs == 0) {
		ldapdb_rdatalist_destroy(ldapdb->common.mctx, &node->rdatalist);
		dns_name_free(&node->owner, ldapdb->common.mctx);
		SAFE_MEM_PUT_PTR(ldapdb->common.mctx, node);
	}

	*targetp = NULL;
}

static isc_result_t
expirenode(dns_db_t *db, dns_dbnode_t *node, isc_stdtime_t now)
{
	UNUSED(db);
	UNUSED(node);
	UNUSED(now);

	return ISC_R_NOTIMPLEMENTED;
}

static void
printnode(dns_db_t *db, dns_dbnode_t *node, FILE *out)
{
	UNUSED(db);
	UNUSED(node);
	UNUSED(out);
}

static isc_result_t
createiterator(dns_db_t *db,
#if LIBDNS_VERSION_MAJOR >= 50
	       unsigned int options,
#else
	       isc_boolean_t relative_names,
#endif
	       dns_dbiterator_t **iteratorp)
{
	isc_result_t result;
	ldap_dbiterator_t *ldapiter = NULL;
	
	CHECKED_MEM_GET_PTR(db->mctx, ldapiter);
	ZERO_PTR(ldapiter);
	ldapiter->common.magic = DNS_DBITERATOR_MAGIC;
	ldapiter->common.methods = &dbiterator_methods;
	attach(db, &ldapiter->common.db);
#if LIBDNS_VERSION_MAJOR >= 50
	UNUSED(options);
#else
	ldapiter->common.relative_names = relative_names;
#endif

	ldapdb_t *ldapdb = (ldapdb_t *) db;
	result = ldapdb_nodelist_get(ldapdb->common.mctx, ldapdb->ldap_inst,
	                             &ldapdb->common.origin, &ldapdb->common.origin,
	                             &ldapiter->nodelist);

	*iteratorp = (dns_dbiterator_t *) ldapiter;

	/* 
	 * ISC_R_NOTFOUND is OK, because SOA record is stored in zone entry.
	 */
	if (result == ISC_R_SUCCESS || result == ISC_R_NOTFOUND)
		return (ISC_R_SUCCESS);

cleanup:
	if (ldapiter != NULL)
		dbiterator_destroy((dns_dbiterator_t **) &ldapiter);
	return result;
}

/*
 * Implementation of DB Iterator methods.
 * @{
 */

static void
dbiterator_destroy(dns_dbiterator_t **iteratorp)
{
	dns_dbiterator_t *iterator = *iteratorp;
	ldap_dbiterator_t *ldapiter = (ldap_dbiterator_t *) iterator;
	dns_db_t *db;

	REQUIRE(iteratorp != NULL && *iteratorp != NULL);

	db = iterator->db;

	ldapiter->current = HEAD(ldapiter->nodelist);
	while (ldapiter->current != NULL) {
		dns_dbnode_t *node = (dns_dbnode_t *) ldapiter->current;
		ldapiter->current = NEXT(ldapiter->current, link);
		detachnode(db, &node);
	}

	SAFE_MEM_PUT_PTR(db->mctx, ldapiter);
	*iteratorp = NULL;
	detach(&db);
}

static isc_result_t
dbiterator_first(dns_dbiterator_t *iterator)
{
	ldap_dbiterator_t *ldapiter = (ldap_dbiterator_t *) iterator;
	
	if (EMPTY(ldapiter->nodelist))
		return (ISC_R_NOMORE);
	
	ldapiter->current = HEAD(ldapiter->nodelist);
	return (ISC_R_SUCCESS);
}

static isc_result_t
dbiterator_last(dns_dbiterator_t *iterator)
{
	ldap_dbiterator_t *ldapiter = (ldap_dbiterator_t *) iterator;

	if (EMPTY(ldapiter->nodelist))
		return (ISC_R_NOMORE);
	
	ldapiter->current = TAIL(ldapiter->nodelist);
	return (ISC_R_NOMORE);
}

static isc_result_t
dbiterator_seek(dns_dbiterator_t *iterator, dns_name_t *name)
{
	UNUSED(iterator);
	UNUSED(name);

	log_bug("%s: not implemented", __FUNCTION__);
	return (ISC_R_NOTIMPLEMENTED);
}

static isc_result_t
dbiterator_prev(dns_dbiterator_t *iterator)
{
	ldap_dbiterator_t *ldapiter = (ldap_dbiterator_t *) iterator;
	ldapiter->current = PREV(ldapiter->current, link);
	if (ldapiter->current == NULL)
		return (ISC_R_NOMORE);

	return (ISC_R_NOMORE);
}

static isc_result_t
dbiterator_next(dns_dbiterator_t *iterator)
{
	ldap_dbiterator_t *ldapiter = (ldap_dbiterator_t *) iterator;
	ldapiter->current = NEXT(ldapiter->current, link);
	if (ldapiter->current == NULL)
		return (ISC_R_NOMORE);
	
	return (ISC_R_SUCCESS);
}

static isc_result_t
dbiterator_current(dns_dbiterator_t *iterator, dns_dbnode_t **nodep,
		   dns_name_t *name)
{
	REQUIRE(nodep != NULL && *nodep == NULL);
	
	ldap_dbiterator_t *ldapiter = (ldap_dbiterator_t *) iterator;
	attachnode(iterator->db, ldapiter->current, nodep);
	
	if (name != NULL)
		return (dns_name_copy(&ldapiter->current->owner, name, NULL));
	
	return (ISC_R_SUCCESS);
}

static isc_result_t
dbiterator_pause(dns_dbiterator_t *iterator)
{
	UNUSED(iterator);
	return (ISC_R_SUCCESS);
}

static isc_result_t
dbiterator_origin(dns_dbiterator_t *iterator, dns_name_t *name)
{
	UNUSED(iterator);
	return (dns_name_copy(dns_rootname, name, NULL));
}

/*
 * @}
 */

static isc_result_t
findrdataset(dns_db_t *db, dns_dbnode_t *node, dns_dbversion_t *version,
	     dns_rdatatype_t type, dns_rdatatype_t covers, isc_stdtime_t now,
	     dns_rdataset_t *rdataset, dns_rdataset_t *sigrdataset)
{
	ldapdb_t *ldapdb = (ldapdb_t *) db;
	ldapdb_node_t *ldapdbnode = (ldapdb_node_t *) node;
	dns_rdatalist_t *rdlist = NULL;
	isc_result_t result;

	UNUSED(db);
	UNUSED(now);
	UNUSED(sigrdataset);

	REQUIRE(covers == 0); /* Only meaningful with DNSSEC capable DB*/
	REQUIRE(VALID_LDAPDBNODE(ldapdbnode));

	if (version != NULL) {
		REQUIRE(version == ldapdb_version);
	}

	result = ldapdb_rdatalist_findrdatatype(&ldapdbnode->rdatalist, type,
						&rdlist);
	if (result != ISC_R_SUCCESS)
		return result;

	result = clone_rdatalist_to_rdataset(ldapdb->common.mctx, rdlist,
					     rdataset);

	return result;
}

static isc_result_t
allrdatasets(dns_db_t *db, dns_dbnode_t *node, dns_dbversion_t *version,
	     isc_stdtime_t now, dns_rdatasetiter_t **iteratorp)
{
	ldapdb_rdatasetiter_t *iter;
	isc_result_t result;

	REQUIRE(version == NULL || version == &dummy);

	CHECKED_MEM_GET_PTR(db->mctx, iter);
	iter->common.magic = DNS_RDATASETITER_MAGIC;
	iter->common.methods = &rdatasetiter_methods;
	iter->common.db = db;
	iter->common.node = NULL;
	attachnode(db, node, &iter->common.node);
	iter->common.version = version;
	iter->common.now = now;

	*iteratorp = (dns_rdatasetiter_t *)iter;

	return ISC_R_SUCCESS;

cleanup:
	return result;
}

/*
 * Remove duplicates between rdlists. If rm_from1 == true then remove rdata
 * from the first rdatalist. same rdata are removed from rdlist1 or 2 and are
 * returned in diff.
 */
static void
rdatalist_removedups(dns_rdatalist_t *rdlist1, dns_rdatalist_t *rdlist2,
		     isc_boolean_t rm_from1,
		     dns_rdatalist_t *diff)
{
	dns_rdata_t *rdata1, *rdata2;

	rdata1 = HEAD(rdlist1->rdata);
	while (rdata1 != NULL) {
		rdata2 = HEAD(rdlist2->rdata);
		while (rdata2 != NULL) {
			if (dns_rdata_compare(rdata1, rdata2) != 0) {
				rdata2 = NEXT(rdata2, link);
				continue;
			}
			/* same rdata has been found */
			if (rm_from1) {
				ISC_LIST_UNLINK(rdlist1->rdata, rdata1, link);
				APPEND(diff->rdata, rdata1, link);
			} else {
				ISC_LIST_UNLINK(rdlist2->rdata, rdata2, link);
				APPEND(diff->rdata, rdata2, link);
			}
			break;
		}
		rdata1 = NEXT(rdata1, link);
	}
}

static isc_result_t
addrdataset(dns_db_t *db, dns_dbnode_t *node, dns_dbversion_t *version,
	    isc_stdtime_t now, dns_rdataset_t *rdataset, unsigned int options,
	    dns_rdataset_t *addedrdataset)
{
	ldapdb_node_t *ldapdbnode = (ldapdb_node_t *) node;
	ldapdb_t *ldapdb = (ldapdb_t *) db;
	dns_rdatalist_t *rdlist = NULL, *new_rdlist = NULL;
	dns_rdatalist_t *found_rdlist = NULL;
	dns_rdatalist_t diff;
	isc_result_t result;
	isc_boolean_t rdatalist_exists = ISC_FALSE;
	isc_boolean_t soa_simulated_write = ISC_FALSE;

	UNUSED(now);
	UNUSED(db);
	UNUSED(addedrdataset);

	REQUIRE(VALID_LDAPDBNODE(ldapdbnode));
	/* version == NULL is valid only for cache databases */
	REQUIRE(version == ldapdb_version);
	REQUIRE((options & DNS_DBADD_FORCE) == 0);

	dns_rdatalist_init(&diff);

	result = dns_rdatalist_fromrdataset(rdataset, &rdlist);
	INSIST(result == ISC_R_SUCCESS);
	INSIST(rdlist->rdclass == dns_rdataclass_in);

	CHECK(rdatalist_clone(ldapdb->common.mctx, rdlist, &new_rdlist));

	result = ldapdb_rdatalist_findrdatatype(&ldapdbnode->rdatalist,
						rdlist->type, &found_rdlist);
	if (result == ISC_R_SUCCESS) {
		rdatalist_exists = ISC_TRUE;

		if (rdlist->ttl != found_rdlist->ttl) {
			/*
			 * TODO: support it. When supported handle
			 * DNS_DBADD_EXACTTTL option well.
			 */
			log_error("multiple TTLs for one name are not "
				  "supported");
			result = ISC_R_NOTIMPLEMENTED;
			goto cleanup;
		}

		if ((options & DNS_DBADD_MERGE) != 0 ||
		    (options & DNS_DBADD_EXACT) != 0) {
			rdatalist_removedups(found_rdlist, new_rdlist,
					     ISC_FALSE, &diff);

			if ((options & DNS_DBADD_MERGE) == 0 &&
			    (rdatalist_length(&diff) != 0)) {
				CLEANUP_WITH(DNS_R_NOTEXACT);
			} else {
				free_rdatalist(ldapdb->common.mctx, &diff);
			}
		} else {
			/* Replace existing rdataset */
			free_rdatalist(ldapdb->common.mctx, found_rdlist);
		}
	}

	/* HACK: SOA addition will never fail with DNS_R_UNCHANGED.
	 * This prevents warning from BIND's diff_apply(), it has too strict
	 * checks for us.
	 *
	 * Reason: There is a race condition between SOA serial update
	 * from BIND's update_action() and our persistent search watcher, because
	 * they don't know about each other.
	 * BIND's update_action() changes data with first addrdataset() call and
	 * then changes serial with second addrdataset() call.
	 * It can lead to empty diff if persistent search watcher
	 * incremented serial in meanwhile.
	 */
	if (HEAD(new_rdlist->rdata) == NULL) {
		if (rdlist->type == dns_rdatatype_soa)
			soa_simulated_write = ISC_TRUE;
		else
			CLEANUP_WITH(DNS_R_UNCHANGED);
	} else {
		CHECK(write_to_ldap(&ldapdbnode->owner, ldapdb->ldap_inst, new_rdlist));
	}


	if (addedrdataset != NULL) {
		if (soa_simulated_write) {
			dns_rdataset_clone(rdataset, addedrdataset);
		} else {
			result = dns_rdatalist_tordataset(new_rdlist, addedrdataset);
			/* Use strong condition here, returns only SUCCESS */
			INSIST(result == ISC_R_SUCCESS);
		}
	}

	if (rdatalist_exists) {
		ISC_LIST_APPENDLIST(found_rdlist->rdata, new_rdlist->rdata,
				    link);
		SAFE_MEM_PUT_PTR(ldapdb->common.mctx, new_rdlist);
	} else
		APPEND(ldapdbnode->rdatalist, new_rdlist, link);

	return ISC_R_SUCCESS;

cleanup:
	if (new_rdlist != NULL) {
		free_rdatalist(ldapdb->common.mctx, new_rdlist);
		SAFE_MEM_PUT_PTR(ldapdb->common.mctx, new_rdlist);
	}
	free_rdatalist(ldapdb->common.mctx, &diff);

	return result;
}

static isc_result_t
subtractrdataset(dns_db_t *db, dns_dbnode_t *node, dns_dbversion_t *version,
		 dns_rdataset_t *rdataset, unsigned int options,
		 dns_rdataset_t *newrdataset)
{
	ldapdb_t *ldapdb = (ldapdb_t *) db;
	ldapdb_node_t *ldapdbnode = (ldapdb_node_t *) node;
	dns_rdatalist_t *found_rdlist = NULL;
	dns_rdatalist_t *rdlist;
	dns_rdatalist_t diff;
	isc_result_t result;
	isc_boolean_t delete_node = ISC_FALSE;

	REQUIRE(version == ldapdb_version);

	result = dns_rdatalist_fromrdataset(rdataset, &rdlist);
	/* Use strong condition here, no other value is returned */
	INSIST(result == ISC_R_SUCCESS);

	/* Do we want to use memcpy here? */
	dns_rdatalist_init(&diff);
	diff.rdclass = rdlist->rdclass;
	diff.type = rdlist->type;
	diff.covers = rdlist->covers;
	diff.ttl = rdlist->ttl;

	result = ldapdb_rdatalist_findrdatatype(&ldapdbnode->rdatalist,
						rdlist->type, &found_rdlist);

	if (result == ISC_R_NOTFOUND)
		return DNS_R_NXRRSET;

	/* We found correct type, remove maching rdata */
	rdatalist_removedups(rdlist, found_rdlist, ISC_FALSE, &diff);

	if ((options & DNS_DBSUB_EXACT) != 0 &&
	     rdatalist_length(&diff) != rdatalist_length(rdlist)) {
		/* Not exact match, rollback */
		result = DNS_R_NOTEXACT;
		goto cleanup;
	}

	if (rdatalist_length(&diff) == 0) {
		result = DNS_R_UNCHANGED;
		goto cleanup;
	}

	/*
	 * If there is only one rdatalist in the node with no rdata
	 * it means all resource records associated with the node's DNS
	 * name (owner) was deleted. So delete the whole node from the
	 * LDAP.
	 */
	if (HEAD(ldapdbnode->rdatalist) == TAIL(ldapdbnode->rdatalist) &&
	    HEAD((HEAD(ldapdbnode->rdatalist))->rdata) == NULL)
		delete_node = ISC_TRUE;

	CHECK(remove_from_ldap(&ldapdbnode->owner, ldapdb->ldap_inst, &diff,
			       delete_node));

	if (newrdataset != NULL) {
		result = dns_rdatalist_tordataset(found_rdlist, newrdataset);
		/* Use strong condition here, no other value is returned */
		INSIST(result == ISC_R_SUCCESS);
	}

	free_rdatalist(ldapdb->common.mctx, &diff);

	return ISC_R_SUCCESS;

cleanup:
	/* Roll back changes */
	ISC_LIST_APPENDLIST(found_rdlist->rdata, diff.rdata, link);

	return result;
}

static isc_result_t
deleterdataset(dns_db_t *db, dns_dbnode_t *node, dns_dbversion_t *version,
	       dns_rdatatype_t type, dns_rdatatype_t covers)
{
	UNUSED(db);
	UNUSED(node);
	UNUSED(version);
	UNUSED(type);
	UNUSED(covers);

	REQUIRE("deleterdataset" == NULL);

	return ISC_R_NOTIMPLEMENTED;
}

static isc_boolean_t
issecure(dns_db_t *db)
{
	UNUSED(db);

	return ISC_FALSE;
}

static unsigned int
nodecount(dns_db_t *db)
{
	UNUSED(db);

	return ISC_R_NOTIMPLEMENTED;
}

/**
 * Return TRUE, because database does not need to be loaded from disk
 * or written to disk.
 */
static isc_boolean_t
ispersistent(dns_db_t *db)
{
	UNUSED(db);

	return ISC_TRUE;
}

static void
overmem(dns_db_t *db, isc_boolean_t overmem)
{
	UNUSED(db);
	UNUSED(overmem);
}

static void
settask(dns_db_t *db, isc_task_t *task)
{
	UNUSED(db);
	UNUSED(task);
}

#if LIBDNS_VERSION_MAJOR >= 31
static isc_result_t
getoriginnode(dns_db_t *db, dns_dbnode_t **nodep)
{
	ldapdb_t *ldapdb = (ldapdb_t *) db;

	return findnode(db, &ldapdb->common.origin, ISC_FALSE, nodep);
}
#endif

#if LIBDNS_VERSION_MAJOR >= 50
static isc_result_t
findnsec3node(dns_db_t *db, dns_name_t *name, isc_boolean_t create,
	      dns_dbnode_t **nodep)
{
	UNUSED(db);
	UNUSED(name);
	UNUSED(create);
	UNUSED(nodep);

	return ISC_R_NOTIMPLEMENTED;
}

static isc_boolean_t
isdnssec(dns_db_t *db)
{
	UNUSED(db);

	return ISC_R_NOTIMPLEMENTED;
}
#endif /* LIBDNS_VERSION_MAJOR >= 50 */

static dns_dbmethods_t ldapdb_methods = {
	attach,
	detach,
	beginload,
	endload,
	dump,
	currentversion,
	newversion,
	attachversion,
	closeversion,
	findnode,
	find,
	findzonecut,
	attachnode,
	detachnode,
	expirenode,
	printnode,
	createiterator,
	findrdataset,
	allrdatasets,
	addrdataset,
	subtractrdataset,
	deleterdataset,
	issecure,
	nodecount,
	ispersistent,
	overmem,
	settask,
#if LIBDNS_VERSION_MAJOR >= 31
	getoriginnode,
#endif
#if LIBDNS_VERSION_MAJOR >= 45
	NULL,			/* transfernode */
#if LIBDNS_VERSION_MAJOR >= 50
	NULL,			/* getnsec3parameters */
	findnsec3node,
	NULL,			/* setsigningtime */
	NULL,			/* getsigningtime */
	NULL,			/* resigned */
	isdnssec,
#endif /* LIBDNS_VERSION_MAJOR >= 50 */
	NULL,			/* getrrsetstats */
#endif /* LIBDNS_VERSION_MAJOR >= 45 */
#if LIBDNS_VERSION_MAJOR >= 82
	NULL,			/* rpz_enabled */
	NULL,			/* rpz_findips */
#endif /* LIBDNS_VERSION_MAJOR >= 82 */
#if LIBDNS_VERSION_MAJOR >= 90
	NULL,			/* findnodeext */
	NULL			/* findext */
#endif /* LIBDNS_VERSION_MAJOR >= 90 */
};

static isc_result_t
ldapdb_create(isc_mem_t *mctx, dns_name_t *name, dns_dbtype_t type,
	      dns_rdataclass_t rdclass, unsigned int argc, char *argv[],
	      void *driverarg, dns_db_t **dbp)
{
	ldapdb_t *ldapdb = NULL;
	isc_result_t result;

	UNUSED(driverarg); /* Currently we don't need any data */

	/* Database instance name. */
	REQUIRE(argc > 0);

	REQUIRE(type == dns_dbtype_zone);
	REQUIRE(rdclass == dns_rdataclass_in);
	REQUIRE(dbp != NULL && *dbp == NULL);

	CHECKED_MEM_GET_PTR(mctx, ldapdb);
	ZERO_PTR(ldapdb);

	isc_mem_attach(mctx, &ldapdb->common.mctx);

	dns_name_init(&ldapdb->common.origin, NULL);
	isc_ondestroy_init(&ldapdb->common.ondest);

	ldapdb->common.magic = DNS_DB_MAGIC;
	ldapdb->common.impmagic = LDAPDB_MAGIC;

	ldapdb->common.methods = &ldapdb_methods;
	ldapdb->common.attributes = 0;
	ldapdb->common.rdclass = rdclass;

	CHECK(dns_name_dupwithoffsets(name, mctx, &ldapdb->common.origin));

	CHECK(isc_refcount_init(&ldapdb->refs, 1));
	CHECK(manager_get_ldap_instance(argv[0], &ldapdb->ldap_inst));

	*dbp = (dns_db_t *)ldapdb;

	return ISC_R_SUCCESS;

cleanup:
	if (ldapdb != NULL) {
		if (dns_name_dynamic(&ldapdb->common.origin))
			dns_name_free(&ldapdb->common.origin, mctx);

		isc_mem_putanddetach(&ldapdb->common.mctx, ldapdb,
				     sizeof(*ldapdb));
	}

	return result;
}

static dns_dbimplementation_t *ldapdb_imp;
const char *ldapdb_impname = "dynamic-ldap";


VISIBLE isc_result_t
dynamic_driver_init(isc_mem_t *mctx, const char *name, const char * const *argv,
		    dns_dyndb_arguments_t *dyndb_args)
{
	dns_dbimplementation_t *ldapdb_imp_new = NULL;
	isc_result_t result;

	REQUIRE(name != NULL);
	REQUIRE(argv != NULL);
	REQUIRE(dyndb_args != NULL);

	log_debug(2, "registering dynamic ldap driver for %s.", name);

	/*
	 * We need to discover what rdataset methods does
	 * dns_rdatalist_tordataset use. We then make a copy for ourselves
	 * with the exception that we modify the disassociate method to free
	 * the rdlist we allocate for it in clone_rdatalist_to_rdataset().
	 */
	if (rdataset_methods.disassociate == NULL) {
		dns_rdataset_t rdset;
		dns_rdatalist_t rdatalist;

		dns_rdataset_init(&rdset);
		dns_rdatalist_tordataset(&rdatalist, &rdset);
		memcpy(&rdataset_methods, rdset.methods,
		       sizeof(dns_rdatasetmethods_t));
		rdataset_methods.disassociate = ldapdb_rdataset_disassociate;
		rdataset_methods.clone = ldapdb_rdataset_clone;
	}

	/* Register new DNS DB implementation. */
	result = dns_db_register(ldapdb_impname, &ldapdb_create, NULL, mctx,
				 &ldapdb_imp_new);
	if (result != ISC_R_SUCCESS && result != ISC_R_EXISTS)
		return result;
	else if (result == ISC_R_SUCCESS)
		ldapdb_imp = ldapdb_imp_new;

	/* Finally, create the instance. */
	result = manager_create_db_instance(mctx, name, argv, dyndb_args);

	return result;
}

VISIBLE void
dynamic_driver_destroy(void)
{
	/* Only unregister the implementation if it was registered by us. */
	if (ldapdb_imp != NULL)
		dns_db_unregister(&ldapdb_imp);

	destroy_manager();
}
