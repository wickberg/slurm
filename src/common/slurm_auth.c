/*****************************************************************************\
 *  slurm_auth.h - implementation-independent authentication API definitions
 *****************************************************************************
 *  Copyright (C) 2002 The Regents of the University of California.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Kevin Tew <tew1@llnl.gov> et. al.
 *  UCRL-CODE-2002-040.
 *  
 *  This file is part of SLURM, a resource management program.
 *  For details, see <http://www.llnl.gov/linux/slurm/>.
 *  
 *  SLURM is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *  
 *  SLURM is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *  
 *  You should have received a copy of the GNU General Public License along
 *  with SLURM; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA  02111-1307  USA.
\*****************************************************************************/

#include <stdlib.h>
#include <string.h>

#include <pthread.h>

#include "src/common/macros.h"
#include "src/common/xmalloc.h"
#include "src/common/xassert.h"
#include "src/common/xstring.h"
#include "src/common/read_config.h"
#include "src/common/slurm_auth.h"
#include "src/common/plugin.h"
#include "src/common/plugrack.h"

/*
 * WARNING:  Do not change the order of these fields or add additional
 * fields at the beginning of the structure.  If you do, authentication
 * plugins will stop working.  If you need to add fields, add them at the
 * end of the structure.
 */
typedef struct slurm_auth_ops {
	void *	(*alloc)	( void );
	void	(*free)		( void *cred );
	int		(*activate)	( void *cred, int secs );
	int		(*verify)	( void *cred );
	uid_t	(*get_uid)	( void *cred );
	gid_t	(*get_gid)	( void *cred );
	void	(*pack)		( void *cred, Buf buf );
	int		(*unpack)	( void *cred, Buf buf );
	void	(*print)	( void *cred, FILE *fp );
} slurm_auth_ops_t;

/*
 * Implementation of the authentication context.  Hopefully everything
 * having to do with plugins will be abstracted under here so that the
 * callers can just deal with creating a context and asking for the
 * operations implemented pertinent to that context.
 *
 * auth_type - the string (presumably from configuration files)
 * describing the desired form of authentication, such as "auth/munged"
 * or "auth/kerberos" or "auth/none".
 *
 * plugin_list - the plugin rack managing the loading and unloading of
 * plugins for authencation.
 *
 * cur_plugin - the plugin currently supplying operations to the caller.
 *
 * ops - a table of pointers to functions in the plugin which correspond
 * to the standardized plugin API.  We create this table by text references
 * into the plugin's symbol table.
 */
struct slurm_auth_context {
	char *auth_type;
	plugrack_t plugin_list;
	plugin_handle_t	cur_plugin;
	slurm_auth_ops_t ops;
};

/*
 * A global authentication context.  "Global" in the sense that there's
 * only one, with static bindings.  We don't export it.
 */
static slurm_auth_context_t g_context = NULL;

static slurm_ctl_conf_t conf;
static pthread_mutex_t config_lock = PTHREAD_MUTEX_INITIALIZER;

static char *
get_plugin_dir( void )
{
	slurm_mutex_lock( &config_lock );
	if ( conf.slurmd_port == 0 ) {
		read_slurm_conf_ctl( &conf );
	}
	if ( conf.plugindir == NULL ) {
		conf.plugindir = xstrdup( "/usr/local/lib" );
	}
	slurm_mutex_unlock( &config_lock );
	
	return conf.plugindir;
}

static char *
get_auth_type( void )
{
	slurm_mutex_lock( &config_lock );
	if ( conf.slurmd_port == 0 ) {
		read_slurm_conf_ctl( &conf );
	}
	if ( conf.authtype == NULL ) {
		conf.authtype = xstrdup( "auth/none" );
	}
	slurm_mutex_unlock( &config_lock );

	return conf.authtype;
}


/*
 * Resolve the operations from the plugin.
 */
static slurm_auth_ops_t *
slurm_auth_get_ops( slurm_auth_context_t c )
{
	/*
	 * These strings must be kept in the same order as the fields
	 * declared for slurm_auth_ops_t.
	 */
	static const char *syms[] = {
		"slurm_auth_alloc",
		"slurm_auth_free",
		"slurm_auth_activate",
		"slurm_auth_verify",
		"slurm_auth_get_uid",
		"slurm_auth_get_gid",
		"slurm_auth_pack",
		"slurm_auth_unpack",
		"slurm_auth_print"
	};
	int n_syms = sizeof( syms ) / sizeof( char * );

	/* Get the plugin list, if needed. */
	if ( c->plugin_list == NULL ) {
		c->plugin_list = plugrack_create();
		if ( c->plugin_list == NULL ) {
			verbose( "Unable to create a plugin manager" );
			return NULL;
		}

		plugrack_set_major_type( c->plugin_list, "auth" );
		plugrack_set_paranoia( c->plugin_list, PLUGRACK_PARANOIA_NONE, 0 );
		plugrack_read_dir( c->plugin_list, get_plugin_dir() );
	}
  
	/* Find the correct plugin. */
	c->cur_plugin = plugrack_use_by_type( c->plugin_list, c->auth_type );
	if ( c->cur_plugin == PLUGIN_INVALID_HANDLE ) {
		verbose( "can't find a plugin for type %s", c->auth_type );
		return NULL;
	}  

	/* Dereference the API. */
	if ( plugin_get_syms( c->cur_plugin,
						  n_syms,
						  syms,
						  (void **) &c->ops ) < n_syms ) {
		verbose( "incomplete plugin detected" );
		return NULL;
	}

	return &c->ops;
}


slurm_auth_context_t
slurm_auth_context_create( const char *auth_type )
{
	slurm_auth_context_t c;
  
	if ( auth_type == NULL ) {
		debug( "slurm_auth_context_create: no authentication type" );
		return NULL;
	}

	c = (slurm_auth_context_t) xmalloc( sizeof( struct slurm_auth_context ) );

	/* Copy the authentication type. */
	c->auth_type = strdup( auth_type );
	if ( c->auth_type == NULL ) {
		debug( "can't make local copy of authentication type" );
		xfree( c );
		return NULL;
	}

	/* Plugin rack is demand-loaded on first reference. */
	c->plugin_list = NULL;

	c->cur_plugin = PLUGIN_INVALID_HANDLE;  

	return c;
}

int
slurm_auth_context_destroy( slurm_auth_context_t c )
{    
	/*
	 * Must check return code here because plugins might still
	 * be loaded and active.
	 */
	if ( c->plugin_list ) {
		if ( plugrack_destroy( c->plugin_list ) != SLURM_SUCCESS ) {
			return SLURM_ERROR;
		}
	}  

	free( c->auth_type );
	xfree( c );
	
	return SLURM_SUCCESS;
}

int
slurm_auth_init( void )
{
	if ( g_context ) {
		return SLURM_SUCCESS;
	}

	
	g_context = slurm_auth_context_create( get_auth_type() );
	if ( g_context == NULL ) {
		verbose( "cannot create a context for %s", get_auth_type() );
		return SLURM_ERROR;
	}
	
	if ( slurm_auth_get_ops( g_context ) == NULL ) {
		verbose( "cannot resolve plugin operations" );
		return SLURM_ERROR;
	} else {
		return SLURM_SUCCESS;
	}
}

/*
 * Static bindings for an arbitrary authentication context.  Heaven
 * help you if you try to pass credentials from one context to the
 * functions for a different context.
 */
void *
c_slurm_auth_alloc( slurm_auth_context_t c )
{
	if ( c == NULL ) return NULL;
	if ( c->ops.alloc ) {
		return (*(c->ops.alloc))();
	} else {
		return NULL;
	}
}

void
c_slurm_auth_free( slurm_auth_context_t c, void *cred )
{
	if ( ( c == NULL ) || ( cred == NULL ) ) return;
	if ( c->ops.free ) {
		(*(c->ops.free))( cred );
	}
}

int
c_slurm_auth_activate( slurm_auth_context_t c, void *cred, int secs )
{
	if ( ( c == NULL ) || ( cred == NULL ) ) return SLURM_ERROR;
	if ( c->ops.activate ) {
		return (*(c->ops.activate))( cred, secs );
	} else {
		return SLURM_ERROR;
	}
}

int
c_slurm_auth_verify( slurm_auth_context_t c, void *cred )
{	
	if ( ( c == NULL ) || ( cred == NULL ) ) return SLURM_ERROR;
	if ( c->ops.verify ) {
		return (*(c->ops.verify))( cred );
	} else {
		return SLURM_ERROR;
	}
}

uid_t
c_slurm_auth_get_uid( slurm_auth_context_t c, void *cred )
{
	if ( ( c == NULL ) || ( cred == NULL ) ) return SLURM_AUTH_NOBODY;
	if ( c->ops.verify ) {
		return (*(c->ops.get_uid))( cred );
	} else {
		return SLURM_AUTH_NOBODY;
	}
}

gid_t
c_slurm_auth_get_gid( slurm_auth_context_t c, void *cred )
{
	if ( ( c == NULL ) || ( cred == NULL ) ) return SLURM_AUTH_NOBODY;
	if ( c->ops.verify ) {
		return (*(c->ops.get_gid))( cred );
	} else {
		return SLURM_AUTH_NOBODY;
	}
}


void
c_slurm_auth_pack( slurm_auth_context_t c, void *cred, Buf buf )
{
	if ( ( c == NULL ) || ( cred == NULL ) || ( buf == NULL ) )
		return;
	if ( c->ops.pack ) {
		(*(c->ops.pack))( cred, buf );
	}
}


int
c_slurm_auth_unpack( slurm_auth_context_t c, void *cred, Buf buf )
{	
	if ( ( c == NULL ) || ( cred == NULL ) || ( buf == NULL ) )
		return SLURM_ERROR;	
	if ( c->ops.unpack ) {
		return (*(c->ops.unpack))( cred, buf );
	} else {
		return SLURM_ERROR;
	}
}

void
c_slurm_auth_print( slurm_auth_context_t c, void *cred, FILE *fp )
{
	if ( ( c == NULL ) || ( cred == NULL ) || ( fp == NULL ) )
		return;
	if ( c->ops.print ) {
		(*(c->ops.print))( cred, fp );
	}
}


/*
 * Static bindings for the global authentication context.  The test
 * of the function pointers is omitted here because the global
 * context initialization includes a test for the completeness of
 * the API function dispatcher.
 */
	  
void *
g_slurm_auth_alloc( void )
{
	if ( ! g_context ) {
		if ( slurm_auth_init() != SLURM_SUCCESS ) {
			error( "can't allocate credential - authentication init failed" );
			return NULL;
		}
	}

	return (*(g_context->ops.alloc))();
}

void
g_slurm_auth_free( void *cred )
{
	if ( ! g_context ) {
		if ( slurm_auth_init() != SLURM_SUCCESS ) {
			error( "can't free credential - authentication init failed" );
			return;
		}
	}

	(*(g_context->ops.free))( cred );
}

int
g_slurm_auth_activate( void *cred, int secs )
{
	if ( ! g_context ) {
		if ( slurm_auth_init() != SLURM_SUCCESS ) {
			error( "can't activate credential - authentication init failed" );
			return SLURM_ERROR;
		}
	}

	return (*(g_context->ops.activate))( cred, secs );
}

int
g_slurm_auth_verify( void *cred )
{
	if ( ! g_context ) {
		if ( slurm_auth_init() != SLURM_SUCCESS ) {
			error( "can't verify credential - authentication init failed" );
			return SLURM_ERROR;
		}
	}
	
	return (*(g_context->ops.verify))( cred );
}

uid_t
g_slurm_auth_get_uid( void *cred )
{
	if ( ! g_context ) {
		if ( slurm_auth_init() != SLURM_SUCCESS ) {
			error( "can't get UID - authentication init failed" );
			return SLURM_AUTH_NOBODY;
		}
	}
	
	return (*(g_context->ops.get_uid))( cred );
}

gid_t
g_slurm_auth_get_gid( void *cred )
{
	if ( ! g_context ) {
		if ( slurm_auth_init() != SLURM_SUCCESS ) {
			error( "can't get GID - authentication init failed" );
			return SLURM_AUTH_NOBODY;
		}
	}
	
	return (*(g_context->ops.get_gid))( cred );
}

void
g_slurm_auth_pack( void *cred, Buf buf )
{
	if ( ! g_context ) {
		if ( slurm_auth_init() != SLURM_SUCCESS ) {
			error( "can't pack credential - authentication init failed" );
			return;
		}
	}
	
	(*(g_context->ops.pack))( cred, buf );
}

int
g_slurm_auth_unpack( void *cred, Buf buf )
{
	if ( ! g_context ) {
		if ( slurm_auth_init() != SLURM_SUCCESS ) {
			error( "can't unpack credential - authentication init failed" );
			return SLURM_ERROR;
		}
	}
	
	return (*(g_context->ops.unpack))( cred, buf );
}

void
g_slurm_auth_print( void *cred, FILE *fp )
{
	if ( ! g_context ) {
		if ( slurm_auth_init() != SLURM_SUCCESS ) {
			error( "can't print credential - authentication init failed" );
			return;
		}
	}
	
	(*(g_context->ops.print))( cred, fp );
}
