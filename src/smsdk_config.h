#pragma once

#define SMEXT_CONF_NAME			"SourceMod JSON Extension"
#define SMEXT_CONF_DESCRIPTION	"Provide JSON Native"
#define SMEXT_CONF_VERSION		"1.1.6"
#define SMEXT_CONF_VERSION_FILE	 1,1,6,0
#define SMEXT_CONF_AUTHOR		"ProjectSky"
#define SMEXT_CONF_URL			"https://github.com/ProjectSky/sm-ext-json"
#define SMEXT_CONF_LOGTAG		"json"
#define SMEXT_CONF_LICENSE		"GPL"
#define SMEXT_CONF_DATESTRING	__DATE__

#define SMEXT_LINK(name) SDKExtension *g_pExtensionIface = name;

#define SMEXT_ENABLE_HANDLESYS
