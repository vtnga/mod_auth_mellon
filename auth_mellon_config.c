/*
 *
 *   auth_mellon_config.c: an authentication apache module
 *   Copyright � 2003-2007 UNINETT (http://www.uninett.no/)
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 * 
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */


#include "auth_mellon.h"

/* This is the default endpoint path. Remember to update the description of
 * the MellonEndpointPath configuration directive if you change this.
 */
static const char *default_endpoint_path = "/mellon/";

/* This is the default name of the attribute we use as a username. Remember
 * to update the description of the MellonUser configuration directive if
 * you change this.
 */
static const char *default_user_attribute = "NAME_ID";

/* This is the default name of the cookie which mod_auth_mellon will set.
 * If you change this, then you should also update the description of the
 * MellonVar configuration directive.
 */
static const char *default_cookie_name = "cookie";

/* The default setting for cookie flags is to not enforce HttpOnly and secure
 */
static const int default_secure_cookie = 0; 

/* The default setting for setting MELLON_SESSION
 */
static const int default_dump_session = 0; 

/* The default setting for setting MELLON_SAML_RESPONSE
 */
static const int default_dump_saml_response = 0; 

/* This is the default IdP initiated login location
 * the MellonDefaultLoginPath configuration directive if you change this.
 */
static const char *default_login_path = "/";

/* This is the directory for storing saved POST sessions
 * the MellonPostDirectory configuration directive if you change this.
 */
static const char *post_dir = "/var/tmp/mellonpost";

/* saved POST session time to live
 * the MellonPostTTL configuration directive if you change this.
 */
static const apr_time_t post_ttl = 15 * 60;

/* saved POST session maximum size
 * the MellonPostSize configuration directive if you change this.
 */
static const apr_size_t post_size = 1024 * 1024 * 1024;

/* maximum saved POST sessions
 * the MellonPostCount configuration directive if you change this.
 */
static const int post_count = 100;

/* This function handles configuration directives which set a 
 * multivalued string slot in the module configuration (the destination
 * strucure is a hash).
 *
 * Parameters:
 *  cmd_parms *cmd       The command structure for this configuration
 *                       directive.
 *  void *struct_ptr     Pointer to the current directory configuration.
 *                       NULL if we are not in a directory configuration.
 *                       This value isn't used by this function.
 *  const char *key      The string argument following this configuration
 *                       directive in the configuraion file.
 *  const char *value    Optional value to be stored in the hash.
 *
 * Returns:
 *  NULL on success or an error string on failure.
 */
static const char *am_set_hash_string_slot(cmd_parms *cmd,
                                          void *struct_ptr,
                                          const char *key,
                                          const char *value)
{
    server_rec *s = cmd->server;
    apr_pool_t *pconf = s->process->pconf;
    am_dir_cfg_rec *cfg = (am_dir_cfg_rec *)struct_ptr;
    int offset;
    apr_hash_t **hash;

    /*
     * If no value is given, we just store the key in the hash.
     */
    if (value == NULL || *value == '\0')
        value = key;

    offset = (int)(long)cmd->info;
    hash = (apr_hash_t **)((char *)cfg + offset);
    apr_hash_set(*hash, apr_pstrdup(pconf, key), APR_HASH_KEY_STRING, value);

    return NULL;
}

/* This function handles configuration directives which set a file
 * slot in the module configuration. If lasso is recent enough, it
 * attempts to read the file immediatly.
 *
 * Parameters:
 *  cmd_parms *cmd       The command structure for this configuration
 *                       directive.
 *  void *struct_ptr     Pointer to the current directory configuration.
 *                       NULL if we are not in a directory configuration.
 *                       This value isn't used by this function.
 *  const char *arg      The string argument following this configuration
 *                       directive in the configuraion file.
 *
 * Returns:
 *  NULL on success or an error string on failure.
 */
static const char *am_set_filestring_slot(cmd_parms *cmd,
                                          void *struct_ptr,
                                          const char *arg)
{
    const char *data;
#ifdef HAVE_lasso_server_new_from_buffers
    if ((data = am_getfile(cmd->pool, cmd->server, arg)) == NULL)
        return apr_psprintf(cmd->pool, "%s - Cannot read file %s",
                            cmd->cmd->name, arg);
#else
    apr_finfo_t finfo;
    apr_status_t rv;
    char error[64];

    rv = apr_stat(&finfo, arg, APR_FINFO_SIZE, cmd->pool);
    if(rv != 0) {
        apr_strerror(rv, error, sizeof(error));
        return apr_psprintf(cmd->pool,
                            "%s - Cannot read file \"%s\" [%d] \"%s\"",
                            cmd->cmd->name, arg, rv, error);
    }

    data = arg;
#endif

    return ap_set_string_slot(cmd, struct_ptr, data);
}


/* This function extracts an IdP ProviderID from metadata
 *
 * Parameters:
 *  apr_pool_t *p        Pool to allocate temporary items from.
 *  server_rec *s        The server.
 *  const char *file     File containing metadata.
 *  const char **provider      The providerID
 *
 * Returns:
 *  NULL on success or an error string on failure.
 *  
 */
static const char *am_get_provider_id(apr_pool_t *p,
                                      server_rec *s,
                                      const char *file,
                                      const char **provider)
{
    char *data;
    apr_xml_parser *xp;
    apr_xml_doc *xd;
    apr_xml_attr *xa;
    char error[1024];

    *provider = NULL;

    /*
     *  Get the data
     */
    if ((data = am_getfile(p, s, file)) == NULL)
        return apr_psprintf(p, "Cannot read file %s", file);

    /* 
     * Parse 
     */
    xp = apr_xml_parser_create(p);
    if (apr_xml_parser_feed(xp, data, strlen(data)) != 0)
        return apr_psprintf(p, "Cannot parse %s: %s", file, 
                            apr_xml_parser_geterror(xp, error, sizeof(error)));

    if (apr_xml_parser_done(xp, &xd) != 0)
        return apr_psprintf(p, "Parse error %s: %s", file, 
                            apr_xml_parser_geterror(xp, error, sizeof(error)));

    /*
     * Extract /EntityDescriptor@EntityID
     */
    if (strcasecmp(xd->root->name, "EntityDescriptor") != 0)
        return apr_psprintf(p, "<EntityDescriptor> is not root in %s", file);

    for (xa = xd->root->attr; xa; xa = xa->next) 
        if (strcasecmp(xa->name, "entityID") == 0)
            break;	

    if (xa  == NULL)
        return apr_psprintf(p, "entityID not found in %s", file);

    *provider = xa->value;
    return NULL;
}

/* This function handles configuration directives which set an 
 * idp related slot in the module configuration. 
 *
 * Parameters:
 *  cmd_parms *cmd       The command structure for this configuration
 *                       directive.
 *  void *struct_ptr     Pointer to the current directory configuration.
 *                       NULL if we are not in a directory configuration.
 *  const char *arg      The string argument following this configuration
 *                       directive in the configuraion file.
 *
 * Returns:
 *  NULL on success or an error string on failure.
 */
static const char *am_set_idp_string_slot(cmd_parms *cmd,
                                          void *struct_ptr,
                                          const char *arg)
{
    server_rec *s = cmd->server;
    apr_pool_t *pconf = s->process->pconf;
    am_dir_cfg_rec *cfg = (am_dir_cfg_rec *)struct_ptr;
    const char *error;
    const char *provider_id;

    if ((error = am_get_provider_id(cmd->pool, s, 
                                    arg, &provider_id)) != NULL)
        return apr_psprintf(cmd->pool, "%s - %s", cmd->cmd->name, error);

    apr_hash_set(cfg->idp_metadata_files,
                 apr_pstrdup(pconf, provider_id),
                 APR_HASH_KEY_STRING,
                 apr_pstrdup(pconf, arg));

    return NULL;
}


/* This function handles configuration directives which set a string
 * slot in the module configuration.
 *
 * Parameters:
 *  cmd_parms *cmd       The command structure for this configuration
 *                       directive.
 *  void *struct_ptr     Pointer to the current directory configuration.
 *                       NULL if we are not in a directory configuration.
 *                       This value isn't used by this function.
 *  const char *arg      The string argument following this configuration
 *                       directive in the configuraion file.
 *
 * Returns:
 *  NULL on success or an error string on failure.
 */
static const char *am_set_module_config_string_slot(cmd_parms *cmd,
                                                    void *struct_ptr,
                                                    const char *arg)
{
    return ap_set_string_slot(cmd, am_get_mod_cfg(cmd->server), arg);
}

/* This function handles configuration directives which set an int
 * slot in the module configuration.
 *
 * Parameters:
 *  cmd_parms *cmd       The command structure for this configuration
 *                       directive.
 *  void *struct_ptr     Pointer to the current directory configuration.
 *                       NULL if we are not in a directory configuration.
 *                       This value isn't used by this function.
 *  const char *arg      The string argument following this configuration
 *                       directive in the configuraion file.
 *
 * Returns:
 *  NULL on success or an error string on failure.
 */
static const char *am_set_module_config_int_slot(cmd_parms *cmd,
                                                 void *struct_ptr,
                                                 const char *arg)
{
    return ap_set_int_slot(cmd, am_get_mod_cfg(cmd->server), arg);
}


/* This function handles the MellonEnable configuration directive.
 * This directive can be set to "off", "info" or "auth".
 *
 * Parameters:
 *  cmd_parms *cmd       The command structure for this configuration
 *                       directive.
 *  void *struct_ptr     Pointer to the current directory configuration.
 *  const char *arg      The string argument following this configuration
 *                       directive in the configuraion file.
 *
 * Returns:
 *  NULL on success or an error string if the argument is wrong.
 */
static const char *am_set_enable_slot(cmd_parms *cmd,
                                      void *struct_ptr,
                                      const char *arg)
{
    am_dir_cfg_rec *d = (am_dir_cfg_rec *)struct_ptr;

    if(!strcasecmp(arg, "auth")) {
        d->enable_mellon = am_enable_auth;
    } else if(!strcasecmp(arg, "info")) {
        d->enable_mellon = am_enable_info;
    } else if(!strcasecmp(arg, "off")) {
        d->enable_mellon = am_enable_off;
    } else {
        return "parameter must be 'off', 'info' or 'auth'";
    }

    return NULL;
}


/* This function handles the MellonDecoder configuration directive.
 * This directive can be set to "none" or "feide".
 *
 * Parameters:
 *  cmd_parms *cmd       The command structure for this configuration
 *                       directive.
 *  void *struct_ptr     Pointer to the current directory configuration.
 *  const char *arg      The string argument following this configuration
 *                       directive in the configuraion file.
 *
 * Returns:
 *  NULL on success or an error string if the argument is wrong.
 */
static const char *am_set_decoder_slot(cmd_parms *cmd,
                                       void *struct_ptr,
                                       const char *arg)
{
    am_dir_cfg_rec *d = (am_dir_cfg_rec *)struct_ptr;

    if(!strcasecmp(arg, "none")) {
        d->decoder = am_decoder_none;
    } else if(!strcasecmp(arg, "feide")) {
        d->decoder = am_decoder_feide;
    } else {
        return "MellonDecoder must be 'none' or 'feide'";
    }

    return NULL;
}


/* This function handles the MellonEndpointPath configuration directive.
 * If the path doesn't end with a '/', then we will append one.
 *
 * Parameters:
 *  cmd_parms *cmd       The command structure for the MellonEndpointPath
 *                       configuration directive.
 *  void *struct_ptr     Pointer to the current directory configuration.
 *                       NULL if we are not in a directory configuration.
 *  const char *arg      The string argument containing the path of the
 *                       endpoint directory.
 *
 * Returns:
 *  This function will always return NULL.
 */
static const char *am_set_endpoint_path(cmd_parms *cmd,
                                        void *struct_ptr,
                                        const char *arg)
{
    am_dir_cfg_rec *d = (am_dir_cfg_rec *)struct_ptr;

    /* Make sure that the path ends with '/'. */
    if(strlen(arg) == 0 || arg[strlen(arg) - 1] != '/') {
        d->endpoint_path = apr_pstrcat(cmd->pool, arg, "/", 0);
    } else {
        d->endpoint_path = arg;
    }

    return NULL;
}


/* This function handles the MellonSetEnv configuration directive.
 * This directive allows the user to change the name of attributes.
 *
 * Parameters:
 *  cmd_parms *cmd       The command structure for the MellonSetEnv
 *                       configuration directive.
 *  void *struct_ptr     Pointer to the current directory configuration.
 *  const char *newName  The new name of the attribute.
 *  const char *oldName  The old name of the attribute.
 *
 * Returns:
 *  This function will always return NULL.
 */
static const char *am_set_setenv_slot(cmd_parms *cmd,
                                      void *struct_ptr,
                                      const char *newName,
                                      const char *oldName)
{
    am_dir_cfg_rec *d = (am_dir_cfg_rec *)struct_ptr;
    apr_hash_set(d->envattr, oldName, APR_HASH_KEY_STRING, newName);
    return NULL;
}

/* This function decodes MellonCond flags, such as [NOT,REG]
 *
 * Parameters:
 *  const char *arg      Pointer to the flags string
 *
 * Returns:
 *  flags, or -1 on error
 */
const char *am_cond_options[] = { 
    "OR",  /* AM_EXPIRE_FLAG_OR */
    "NOT", /* AM_EXPIRE_FLAG_NOT */
    "REG", /* AM_EXPIRE_FLAG_REG */
    "NC",  /* AM_EXPIRE_FLAG_NC */
    "MAP", /* AM_EXPIRE_FLAG_MAP */
    "IGN", /* AM_EXPIRE_FLAG_IGN */
    "REQ", /* AM_EXPIRE_FLAG_REQ */
};

static int am_cond_flags(const char *arg)
{
    int flags = AM_COND_FLAG_NULL; 
    
    /* Skip inital [ */
    if (arg[0] == '[')
        arg++;
    else
        return -1;
 
    do {
        apr_size_t i;

        for (i = 0; i < AM_COND_FLAG_COUNT; i++) {
            apr_size_t optlen = strlen(am_cond_options[i]);

            if (strncmp(arg, am_cond_options[i], optlen) == 0) {
                /* Make sure we have a separator next */
                if (arg[optlen] && !strchr("]\t ,", (int)arg[optlen]))
                       return -1;

                flags |= (1 << i); 
                arg += optlen;
                break;
            }
      
         /* no match */
         if (i == AM_COND_FLAG_COUNT)
             return -1;

         /* skip spaces, tabs and commas */
         arg += strspn(arg, " \t,");

         /* Garbage after ] is ignored */
         if (*arg == ']') 
             return flags;
         }
    } while (*arg);

    /* Missing trailing ] */
    return -1;
}

/* This function handles the MellonCond configuration directive, which
 * allows the user to restrict access based on attributes received from
 * the IdP.
 *
 * Parameters:
 *  cmd_parms *cmd       The command structure for the MellonCond
 *                       configuration directive.
 *  void *struct_ptr     Pointer to the current directory configuration.
 *  const char *attribute   Pointer to the attribute name
 *  const char *value       Pointer to the attribute value or regex
 *  const char *options     Pointer to options
 *
 * Returns:
 *  NULL on success or an error string on failure.
 */
static const char *am_set_cond_slot(cmd_parms *cmd,
                                    void *struct_ptr,
                                    const char *attribute,
                                    const char *value,
                                    const char *options)
{
    am_dir_cfg_rec *d = struct_ptr;
    am_cond_t *element;

    if (*attribute == '\0' || *value == '\0')
        return apr_pstrcat(cmd->pool, cmd->cmd->name,
                           " takes two or three arguments", NULL);
 
    element = (am_cond_t *)apr_array_push(d->cond);
    element->varname = attribute;
    element->flags = AM_COND_FLAG_NULL;
    element->str = NULL;
    element->regex = NULL;
    element->directive = apr_pstrcat(cmd->pool, cmd->directive->directive, 
                                     " ", cmd->directive->args, NULL);

    /* Handle optional flags */
    if (*options != '\0') {
        int flags;

        flags = am_cond_flags(options);
        if (flags == -1)
             return apr_psprintf(cmd->pool, "%s - invalid flags %s",
                                 cmd->cmd->name, options);

        element->flags = flags;
    }

    if (element->flags & AM_COND_FLAG_REG) {
        int regex_flags = AP_REG_EXTENDED|AP_REG_NOSUB;

        if (element->flags & AM_COND_FLAG_NC)
            regex_flags |= AP_REG_ICASE;

        element->regex = ap_pregcomp(cmd->pool, value, regex_flags);
        if (element->regex == NULL) 
             return apr_psprintf(cmd->pool, "%s - invalid regex %s",
                                 cmd->cmd->name, value);
    }

    /*
     * We keep the string also for regex, so that we can 
     * print it for debug purpose.
     */
    element->str = value;
    
    return NULL;
}

/* This function handles the MellonRequire configuration directive, which
 * allows the user to restrict access based on attributes received from
 * the IdP.
 *
 * Parameters:
 *  cmd_parms *cmd       The command structure for the MellonRequire
 *                       configuration directive.
 *  void *struct_ptr     Pointer to the current directory configuration.
 *  const char *arg      Pointer to the configuration string.
 *
 * Returns:
 *  NULL on success or an error string on failure.
 */
static const char *am_set_require_slot(cmd_parms *cmd,
                                       void *struct_ptr,
                                       const char *arg)
{
    am_dir_cfg_rec *d = struct_ptr;
    char *attribute, *value;
    int i;
    am_cond_t *element;
    am_cond_t *first_element;

    attribute = ap_getword_conf(cmd->pool, &arg);
    value     = ap_getword_conf(cmd->pool, &arg);

    if (*attribute == '\0' || *value == '\0') {
        return apr_pstrcat(cmd->pool, cmd->cmd->name,
                           " takes at least two arguments", NULL);
    }

    /*
     * MellonRequire overwrites previous conditions on this attribute
     * We just tag the am_cond_t with the ignore flag, as it is 
     * easier (and probably faster) than to really remove it.
     */
    for (i = 0; i < d->cond->nelts; i++) {
        am_cond_t *ce = &((am_cond_t *)(d->cond->elts))[i];
 
        if ((strcmp(ce->varname, attribute) == 0) && 
            (ce->flags & AM_COND_FLAG_REQ))
            ce->flags |= AM_COND_FLAG_IGN;
    }
    
    first_element = NULL;
    do {
        element = (am_cond_t *)apr_array_push(d->cond);
        element->varname = attribute;
        element->flags = AM_COND_FLAG_OR|AM_COND_FLAG_REQ;
        element->str = value;
        element->regex = NULL;

        /*
         * When multiple values are given, we track the first one
         * in order to retreive the directive
         */ 
        if (first_element == NULL) {
            element->directive = apr_pstrcat(cmd->pool, 
                                             cmd->directive->directive, " ",
                                             cmd->directive->args, NULL);
            first_element = element;
        } else {
            element->directive = first_element->directive;
        }

    } while (*(value = ap_getword_conf(cmd->pool, &arg)) != '\0');

    /* 
     * Remove OR flag on last element 
     */
    element->flags &= ~AM_COND_FLAG_OR;

    return NULL;
}

/* This function handles the MellonOrganization* directives, which
 * which specify language-qualified strings
 *
 * Parameters:
 *  cmd_parms *cmd       The command structure for the MellonOrganization*
 *                       configuration directive.
 *  void *struct_ptr     Pointer to the current directory configuration.
 *  const char *lang     Pointer to the language string (optional)
 *  const char *value    Pointer to the data
 *
 * Returns:
 *  NULL on success or an error string on failure.
 */
static const char *am_set_langstring_slot(cmd_parms *cmd,
                                          void *struct_ptr,
                                          const char *lang,
                                          const char *value)
{
    apr_hash_t *h = *(apr_hash_t **)(struct_ptr + (apr_size_t)cmd->info);

    if (value == NULL || *value == '\0') {
        value = lang;
        lang = "";
    }

    apr_hash_set(h, lang, APR_HASH_KEY_STRING, 
		 apr_pstrdup(cmd->server->process->pconf, value));

    return NULL;
}

/* This array contains all the configuration directive which are handled
 * by auth_mellon.
 */    
const command_rec auth_mellon_commands[] = {

    /* Global configuration directives. */

    AP_INIT_TAKE1(
        "MellonCacheSize",
        am_set_module_config_int_slot,
        (void *)APR_OFFSETOF(am_mod_cfg_rec, cache_size),
        RSRC_CONF,
        "The number of sessions we can keep track of at once. You must"
        " restart the server before any changes to this directive will"
        " take effect. The default value is 100."
        ),
    AP_INIT_TAKE1(
        "MellonLockFile",
        am_set_module_config_string_slot,
        (void *)APR_OFFSETOF(am_mod_cfg_rec, lock_file),
        RSRC_CONF,
        "The lock file for session synchronization."
        " Default value is \"/tmp/mellonLock\"."
        ), 
    AP_INIT_TAKE1(
        "MellonPostDirectory",
        am_set_module_config_string_slot,
        (void *)APR_OFFSETOF(am_mod_cfg_rec, post_dir),
        RSRC_CONF,
        "The directory for saving POST requests."
        " Default value is \"/var/tmp/mellonpost\"."
        ), 
    AP_INIT_TAKE1(
        "MellonPostTTL",
        am_set_module_config_int_slot,
        (void *)APR_OFFSETOF(am_mod_cfg_rec, post_ttl),
        RSRC_CONF,
        "The time to live for saved POST requests in seconds."
        " Default value is 15 mn."
        ), 
    AP_INIT_TAKE1(
        "MellonPostCount",
        am_set_module_config_int_slot,
        (void *)APR_OFFSETOF(am_mod_cfg_rec, post_count),
        RSRC_CONF,
        "The maximum saved POST sessions at once."
        " Default value is 100."
        ), 
    AP_INIT_TAKE1(
        "MellonPostSize",
        am_set_module_config_int_slot,
        (void *)APR_OFFSETOF(am_mod_cfg_rec, post_size),
        RSRC_CONF,
        "The maximum size of a saved POST, in bytes."
        " Default value is 1 MB."
        ), 


    /* Per-location configuration directives. */

    AP_INIT_TAKE1(
        "MellonEnable",
        am_set_enable_slot,
        NULL,
        OR_AUTHCFG,
        "Enable auth_mellon on a location. This can be set to 'off', 'info'"
        " and 'auth'. 'off' disables auth_mellon for a location, 'info'"
        " will only populate the environment with attributes if the user"
        " has logged in already. 'auth' will redirect the user to the IdP"
        " if he hasn't logged in yet, but otherwise behaves like 'info'."
        ),
    AP_INIT_TAKE1(
        "MellonDecoder",
        am_set_decoder_slot,
        NULL,
        OR_AUTHCFG,
        "Select which decoder mod_auth_mellon should use to decode attribute"
        " values. This option can be se to either 'none' or 'feide'. 'none'"
        " is the default, and will store the attributes as they are received"
        " from the IdP. 'feide' is for decoding base64-encoded values which"
        " are separated by a underscore."
        ),
    AP_INIT_TAKE1(
        "MellonVariable",
        ap_set_string_slot,
        (void *)APR_OFFSETOF(am_dir_cfg_rec, varname),
        OR_AUTHCFG,
        "The name of the cookie which auth_mellon will set. Defaults to"
        " 'cookie'. This string is appended to 'mellon-' to create the"
        " cookie name, and the default name of the cookie will therefore"
        " be 'mellon-cookie'."
        ),
    AP_INIT_FLAG(
        "MellonSecureCookie",
        ap_set_flag_slot,
        (void *)APR_OFFSETOF(am_dir_cfg_rec, secure),
        OR_AUTHCFG,
        "Whether the cookie set by auth_mellon should have HttpOnly and"
        " secure flags set. Default is off."
        ),
    AP_INIT_TAKE1(
        "MellonUser",
        ap_set_string_slot,
        (void *)APR_OFFSETOF(am_dir_cfg_rec, userattr),
        OR_AUTHCFG,
        "Attribute to set as r->user. Defaults to NAME_ID, which is the"
        " attribute we set to the identifier we receive from the IdP."
        ),
    AP_INIT_TAKE1(
        "MellonIdP",
        ap_set_string_slot,
        (void *)APR_OFFSETOF(am_dir_cfg_rec, idpattr),
        OR_AUTHCFG,
        "Attribute we set to the IdP ProviderId."
        ),
    AP_INIT_TAKE2(
        "MellonSetEnv",
        am_set_setenv_slot,
        NULL,
        OR_AUTHCFG,
        "Renames attributes received from the server. The format is"
        " MellonSetEnv <old name> <new name>."
        ),
    AP_INIT_FLAG(
        "MellonSessionDump",
        ap_set_flag_slot,
        (void *)APR_OFFSETOF(am_dir_cfg_rec, dump_session),
        OR_AUTHCFG,
        "Dump session in environement. Default is off"
        ),
    AP_INIT_FLAG(
        "MellonSamlResponseDump",
        ap_set_flag_slot,
        (void *)APR_OFFSETOF(am_dir_cfg_rec, dump_saml_response),
        OR_AUTHCFG,
        "Dump SAML authentication response in environement. Default is off"
        ),
    AP_INIT_RAW_ARGS(
        "MellonRequire",
        am_set_require_slot,
        NULL,
        OR_AUTHCFG,
        "Attribute requirements for authorization. Allows you to restrict"
        " access based on attributes received from the IdP. If you list"
        " several MellonRequire configuration directives, then all of them"
        " must match. Every MellonRequire can list several allowed values"
        " for the attribute. The syntax is:"
        " MellonRequire <attribute> <value1> [value2....]."
        ),
    AP_INIT_TAKE23(
        "MellonCond",
        am_set_cond_slot,
        NULL,
        OR_AUTHCFG,
        "Attribute requirements for authorization. Allows you to restrict"
        " access based on attributes received from the IdP. The syntax is:"
        " MellonRequire <attribute> <value> [<options>]."
        ),
    AP_INIT_TAKE1(
        "MellonSessionLength",
        ap_set_int_slot,
        (void *)APR_OFFSETOF(am_dir_cfg_rec, session_length),
        OR_AUTHCFG,
        "Maximum number of seconds a session will be valid for. Defaults"
        " to 86400 seconds (1 day)."
        ),
    AP_INIT_TAKE1(
        "MellonNoCookieErrorPage",
        ap_set_string_slot,
        (void *)APR_OFFSETOF(am_dir_cfg_rec, no_cookie_error_page),
        OR_AUTHCFG,
        "Web page to display if the user has disabled cookies. We will"
        " return a 400 Bad Request error if this is unset and the user"
        " ha disabled cookies."
        ),
    AP_INIT_TAKE1(
        "MellonSPMetadataFile",
        am_set_filestring_slot,
        (void *)APR_OFFSETOF(am_dir_cfg_rec, sp_metadata_file),
        OR_AUTHCFG,
        "Full path to xml file with metadata for the SP."
        ),
    AP_INIT_TAKE1(
        "MellonSPPrivateKeyFile",
        am_set_filestring_slot,
        (void *)APR_OFFSETOF(am_dir_cfg_rec, sp_private_key_file),
        OR_AUTHCFG,
        "Full path to pem file with the private key for the SP."
        ),
    AP_INIT_TAKE1(
        "MellonSPCertFile",
        am_set_filestring_slot,
        (void *)APR_OFFSETOF(am_dir_cfg_rec, sp_cert_file),
        OR_AUTHCFG,
        "Full path to pem file with certificate for the SP."
        ),
    AP_INIT_TAKE1(
        "MellonIdPMetadataFile",
        am_set_idp_string_slot,
        NULL,
        OR_AUTHCFG,
        "Full path to xml metadata file for the IdP."
        ),
    AP_INIT_TAKE1(
        "MellonIdPPublicKeyFile",
        ap_set_string_slot,
        (void *)APR_OFFSETOF(am_dir_cfg_rec, idp_public_key_file),
        OR_AUTHCFG,
        "Full path to pem file with the public key for the IdP."
        ),
    AP_INIT_TAKE1(
        "MellonIdPCAFile",
        ap_set_string_slot,
        (void *)APR_OFFSETOF(am_dir_cfg_rec, idp_ca_file),
        OR_AUTHCFG,
        "Full path to pem file with CA chain for the IdP."
        ),
    AP_INIT_TAKE12(
        "MellonOrganizationName",
        am_set_langstring_slot,
        (void *)APR_OFFSETOF(am_dir_cfg_rec, sp_org_name),
        OR_AUTHCFG,
        "Language-qualified oranization name."
        ),
    AP_INIT_TAKE12(
        "MellonOrganizationDisplayName",
        am_set_langstring_slot,
        (void *)APR_OFFSETOF(am_dir_cfg_rec, sp_org_display_name),
        OR_AUTHCFG,
        "Language-qualified oranization name, human redable."
        ),
    AP_INIT_TAKE12(
        "MellonOrganizationURL",
        am_set_langstring_slot,
        (void *)APR_OFFSETOF(am_dir_cfg_rec, sp_org_url),
        OR_AUTHCFG,
        "Language-qualified oranization URL."
        ),
    AP_INIT_TAKE1(
        "MellonDefaultLoginPath",
        ap_set_string_slot,
        (void *)APR_OFFSETOF(am_dir_cfg_rec, login_path),
        OR_AUTHCFG,
        "The location where to redirect after IdP initiated login."
        " Default value is \"/\"."
        ),
    AP_INIT_TAKE1(
        "MellonDiscoveryURL",
        ap_set_string_slot,
        (void *)APR_OFFSETOF(am_dir_cfg_rec, discovery_url),
        OR_AUTHCFG,
        "The URL of IdP discovery service. Default is unset."
        ),
    AP_INIT_TAKE1(
        "MellonProbeDiscoveryTimeout",
        ap_set_int_slot,
        (void *)APR_OFFSETOF(am_dir_cfg_rec, probe_discovery_timeout),
        OR_AUTHCFG,
        "The timeout of IdP probe discovery service. "
        "Default is 1s"
        ),
    AP_INIT_TAKE12(
        "MellonProbeDiscoveryIdP",
        am_set_hash_string_slot,
        (void *)APR_OFFSETOF(am_dir_cfg_rec, probe_discovery_idp),
        OR_AUTHCFG,
        "An IdP that can be used for IdP probe discovery."
        ),
    AP_INIT_TAKE1(
        "MellonEndpointPath",
        am_set_endpoint_path,
        NULL,
        OR_AUTHCFG,
        "The root directory of the SAML2 endpoints, relative to the root"
        " of the web server. Default value is \"/mellon/\", which will"
        " make mod_mellon to the handler for every request to"
        " \"http://<servername>/mellon/*\". The path you specify must"
        " be contained within the current Location directive."
        ),
    {NULL}
};


/* This function creates and initializes a directory configuration
 * object for auth_mellon.
 *
 * Parameters:
 *  apr_pool_t *p        The pool we should allocate memory from.
 *  char *d              Unused, always NULL.
 *
 * Returns:
 *  The new directory configuration object.
 */
void *auth_mellon_dir_config(apr_pool_t *p, char *d)
{
    am_dir_cfg_rec *dir = apr_palloc(p, sizeof(*dir));

    dir->enable_mellon = am_enable_default;

    dir->decoder = am_decoder_default;

    dir->varname = default_cookie_name;
    dir->secure = default_secure_cookie;
    dir->cond = apr_array_make(p, 0, sizeof(am_cond_t));
    dir->envattr   = apr_hash_make(p);
    dir->userattr  = default_user_attribute;
    dir->idpattr  = NULL;
    dir->dump_session = default_dump_session;
    dir->dump_saml_response = default_dump_saml_response;

    dir->endpoint_path = default_endpoint_path;

    dir->session_length = -1; /* -1 means use default. */

    dir->no_cookie_error_page = NULL;

    dir->sp_metadata_file = NULL;
    dir->sp_private_key_file = NULL;
    dir->sp_cert_file = NULL;
    dir->idp_metadata_files = apr_hash_make(p);
    dir->idp_public_key_file = NULL;
    dir->idp_ca_file = NULL;
    dir->login_path = default_login_path;
    dir->discovery_url = NULL;
    dir->probe_discovery_timeout = -1; /* -1 means no probe discovery */
    dir->probe_discovery_idp = apr_hash_make(p);

    dir->sp_org_name = apr_hash_make(p);
    dir->sp_org_display_name = apr_hash_make(p);
    dir->sp_org_url = apr_hash_make(p);

    apr_thread_mutex_create(&dir->server_mutex, APR_THREAD_MUTEX_DEFAULT, p);

    dir->server = NULL;

    return dir;
}


/* This function merges two am_dir_cfg_rec structures.
 * It will try to inherit from the base where possible.
 *
 * Parameters:
 *  apr_pool_t *p        The pool we should allocate memory from.
 *  void *base           The original structure.
 *  void *add            The structure we should add to base.
 *
 * Returns:
 *  The merged structure.
 */
void *auth_mellon_dir_merge(apr_pool_t *p, void *base, void *add)
{
    am_dir_cfg_rec *base_cfg = (am_dir_cfg_rec *)base;
    am_dir_cfg_rec *add_cfg = (am_dir_cfg_rec *)add;
    am_dir_cfg_rec *new_cfg;

    new_cfg = (am_dir_cfg_rec *)apr_palloc(p, sizeof(*new_cfg));


    new_cfg->enable_mellon = (add_cfg->enable_mellon != am_enable_default ?
                              add_cfg->enable_mellon :
                              base_cfg->enable_mellon);


    new_cfg->decoder = (add_cfg->decoder != am_decoder_default ?
                        add_cfg->decoder :
                        base_cfg->decoder);


    new_cfg->varname = (add_cfg->varname != default_cookie_name ?
                        add_cfg->varname :
                        base_cfg->varname);

    
    new_cfg->secure = (add_cfg->secure != default_secure_cookie ?
                        add_cfg->secure :
                        base_cfg->secure);


    new_cfg->cond = apr_array_copy(p,
                                   (!apr_is_empty_array(add_cfg->cond)) ?
                                   add_cfg->cond :
                                   base_cfg->cond);

    new_cfg->envattr = apr_hash_copy(p,
                                     (apr_hash_count(add_cfg->envattr) > 0) ?
                                     add_cfg->envattr :
                                     base_cfg->envattr);

    new_cfg->userattr = (add_cfg->userattr != default_user_attribute ?
                         add_cfg->userattr :
                         base_cfg->userattr);

    new_cfg->idpattr = (add_cfg->idpattr != NULL ?
                        add_cfg->idpattr :
                        base_cfg->idpattr);

    new_cfg->dump_session = (add_cfg->dump_session != default_dump_session ?
                             add_cfg->dump_session :
                             base_cfg->dump_session);

    new_cfg->dump_saml_response = 
        (add_cfg->dump_saml_response != default_dump_saml_response ?
         add_cfg->dump_saml_response :
         base_cfg->dump_saml_response);

    new_cfg->endpoint_path = (
        add_cfg->endpoint_path != default_endpoint_path ?
        add_cfg->endpoint_path :
        base_cfg->endpoint_path
        );

    new_cfg->session_length = (add_cfg->session_length != -1 ?
                               add_cfg->session_length :
                               base_cfg->session_length);

    new_cfg->no_cookie_error_page = (add_cfg->no_cookie_error_page != NULL ?
                                     add_cfg->no_cookie_error_page :
                                     base_cfg->no_cookie_error_page);


    new_cfg->sp_metadata_file = (add_cfg->sp_metadata_file ?
                                 add_cfg->sp_metadata_file :
                                 base_cfg->sp_metadata_file);

    new_cfg->sp_private_key_file = (add_cfg->sp_private_key_file ?
                                    add_cfg->sp_private_key_file :
                                    base_cfg->sp_private_key_file);

    new_cfg->sp_cert_file = (add_cfg->sp_cert_file ?
                             add_cfg->sp_cert_file :
                             base_cfg->sp_cert_file);

    new_cfg->idp_metadata_files = apr_hash_copy(p,
                         (apr_hash_count(add_cfg->idp_metadata_files) > 0) ?
                         add_cfg->idp_metadata_files :
                         base_cfg->idp_metadata_files);

    new_cfg->idp_public_key_file = (add_cfg->idp_public_key_file ?
                                    add_cfg->idp_public_key_file :
                                    base_cfg->idp_public_key_file);

    new_cfg->idp_ca_file = (add_cfg->idp_ca_file ?
                            add_cfg->idp_ca_file :
                            base_cfg->idp_ca_file);

    new_cfg->sp_org_name = apr_hash_copy(p,
                          (apr_hash_count(add_cfg->sp_org_name) > 0) ?
                           add_cfg->sp_org_name : 
                           base_cfg->sp_org_name);

    new_cfg->sp_org_display_name = apr_hash_copy(p,
                          (apr_hash_count(add_cfg->sp_org_display_name) > 0) ?
                           add_cfg->sp_org_display_name : 
                           base_cfg->sp_org_display_name);

    new_cfg->sp_org_url = apr_hash_copy(p,
                          (apr_hash_count(add_cfg->sp_org_url) > 0) ?
                           add_cfg->sp_org_url : 
                           base_cfg->sp_org_url);

    new_cfg->login_path = (add_cfg->login_path != default_login_path ?
                           add_cfg->login_path :
                           base_cfg->login_path);

    new_cfg->discovery_url = (add_cfg->discovery_url ?
                              add_cfg->discovery_url :
                              base_cfg->discovery_url);

    new_cfg->probe_discovery_timeout = 
                           (add_cfg->probe_discovery_timeout != -1 ?
                            add_cfg->probe_discovery_timeout :
                            base_cfg->probe_discovery_timeout);

    new_cfg->probe_discovery_idp = apr_hash_copy(p,
                      (apr_hash_count(add_cfg->probe_discovery_idp) > 0) ?
                       add_cfg->probe_discovery_idp : 
                       base_cfg->probe_discovery_idp);

    apr_thread_mutex_create(&new_cfg->server_mutex,
                            APR_THREAD_MUTEX_DEFAULT, p);
    new_cfg->server = NULL;

    return new_cfg;
}


/* This function creates a new per-server configuration.
 * auth_mellon uses the server configuration to store a pointer
 * to the global module configuration.
 *
 * Parameters:
 *  apr_pool_t *p        The pool we should allocate memory from.
 *  server_rec *s        The server we should add our configuration to.
 *
 * Returns:
 *  The new per-server configuration.
 */
void *auth_mellon_server_config(apr_pool_t *p, server_rec *s)
{
    am_srv_cfg_rec *srv;
    am_mod_cfg_rec *mod;
    const char key[] = "auth_mellon_server_config";

    srv = apr_palloc(p, sizeof(*srv));

    /* we want to keeep our global configuration of shared memory and
     * mutexes, so we try to find it in the userdata before doing anything
     * else */
    apr_pool_userdata_get((void **)&mod, key, p);
    if (mod) {
        srv->mc = mod;
        return srv;
    }

    /* the module has not been initiated at all */
    mod = apr_palloc(p, sizeof(*mod));

    mod->cache_size = 100;  /* ought to be enough for everybody */
    mod->lock_file  = "/tmp/mellonLock";
    mod->post_dir   = post_dir;
    mod->post_ttl   = post_ttl;
    mod->post_count = post_count;
    mod->post_size  = post_size;

    mod->init_cache_size = 0;
    mod->init_lock_file = NULL;

    mod->cache      = NULL;
    mod->lock       = NULL;

    apr_pool_userdata_set(mod, key, apr_pool_cleanup_null, p);

    srv->mc = mod;
    return srv;
}

