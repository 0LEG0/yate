/**
 * regfile.cpp
 * This file is part of the YATE Project http://YATE.null.ro
 *
 * Ask for a registration from this module.
 *
 * Yet Another Telephony Engine - a fully featured software PBX and IVR
 * Copyright (C) 2004-2010 Null Team
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include <yatengine.h>

using namespace TelEngine;
namespace { // anonymous


class AuthHandler : public MessageHandler
{
public:
    AuthHandler(const char *name, unsigned prio = 100)
	: MessageHandler(name,prio) { }
    virtual bool received(Message &msg);
};

class RegistHandler : public MessageHandler
{
public:
    RegistHandler(const char *name, unsigned prio = 100)
	: MessageHandler(name,prio) { }
    virtual bool received(Message &msg);
};

class UnRegistHandler : public MessageHandler
{
public:
    UnRegistHandler(const char *name, unsigned prio = 100)
	: MessageHandler(name,prio) { }
    virtual bool received(Message &msg);
};

class RouteHandler : public MessageHandler
{
public:
    RouteHandler(const char *name, unsigned prio = 100);
    ~RouteHandler();
    virtual bool received(Message &msg);
private:
    ObjList* m_skip;
};

class StatusHandler : public MessageHandler
{
public:
    StatusHandler(const char *name, unsigned prio = 100)
	: MessageHandler(name,prio) { }
    virtual bool received(Message &msg);
};

class ExpireHandler : public MessageHandler
{
public:
    ExpireHandler()
	: MessageHandler("engine.timer",100) { }
    virtual bool received(Message &msg);
};

class ExpandedUser : public ObjList
{
public:
    inline ExpandedUser(const String& username)
	: m_username(username) {}
    virtual const String& toString () const
	{ return m_username; }
private:
    String m_username;
};

class RegfilePlugin : public Plugin
{
public:
    RegfilePlugin();
    ~RegfilePlugin();
    virtual void initialize();
    void populate();
private:
    bool m_init;
};


Mutex s_mutex(false,"RegFile");

static Configuration s_cfg(Engine::configFile("regfile"));
static Configuration s_accounts;
static bool s_create = false;
static const String s_general = "general";
static ObjList s_expand;

bool expired(const NamedList& list, unsigned int time)
{
    // If eTime is 0 the registration will never expire
    unsigned int eTime = list.getIntValue("expires",0);
    return eTime && eTime < time;
}

bool AuthHandler::received(Message &msg)
{
    String username(msg.getValue("username"));
    if (username.null() || username == s_general)
	return false;
    Lock lock(s_mutex);
    const NamedList* usr = s_cfg.getSection(username);
    if (!usr)
	return false;
    const String* pass = usr->getParam("password");
    if (!pass)
	return false;
    msg.retValue() = *pass;
    Debug("RegFile",DebugAll,"Authenticating user %s with password length %u",
	username.c_str(),pass->length());
    return true;
}

bool RegistHandler::received(Message &msg)
{
    String username(msg.getValue("username"));
    if (username.null() || username == s_general)
	return false;
    const char* driver = msg.getValue("driver");
    const char* data = msg.getValue("data");
    if (!data)
	return false;
    Lock lock(s_mutex);
    int expire = msg.getIntValue("expires",0);
    NamedList* sect = s_cfg.getSection(username);
    if (s_create && !sect) {
	sect = new NamedList(username);
	Debug("RegFile",DebugInfo,"Auto creating new user %s",username.c_str());
    }
    if (!sect)
	return false;
    s_accounts.createSection(*sect);
    NamedList* s = s_accounts.getSection(*sect);
    if (driver)
	s->setParam("driver",driver);
    s->setParam("data",data);
    if (expire)
	s->setParam("expires",String(msg.msgTime().sec() + expire));
    Debug("RegFile",DebugAll,"Registered user %s via %s",username.c_str(),data);
    return true;
}

bool UnRegistHandler::received(Message &msg)
{
    String username(msg.getValue("username"));
    if (username.null() || username == s_general)
	return false;
    Lock lock(s_mutex);
    NamedList* nl = s_accounts.getSection(username);
    if (!nl)
	return false;
    Debug("RegFile",DebugAll,"Removing user %s, reason unregistered",username.c_str());
    s_accounts.clearSection(username);
    return true;
}

RouteHandler::RouteHandler(const char *name, unsigned prio)
    : MessageHandler(name,prio)
{
    m_skip = String("alternatives,password").split(',');
}

RouteHandler::~RouteHandler()
{
    TelEngine::destruct(m_skip);
}

bool RouteHandler::received(Message &msg)
{
    String user = msg.getValue("caller");
    Lock lock(s_mutex);
    NamedList* params = 0;
    if (user) {
	params = s_cfg.getSection(user);
	if (params) {
	    unsigned int n = params->length();
	    for (unsigned int i = 0; i < n; i++) {
		const NamedString* s = params->getParam(i);
		if (s && !m_skip->find(s->name())) {
		    String value = *s;
		    msg.replaceParams(value);
		    msg.setParam(s->name(),value);
		}
	    }
	}
    }

    String username(msg.getValue("called"));
    if (username.null() || username == s_general)
	return false;
    NamedList* ac = s_accounts.getSection(username);
    while (true) {
	String data;
	if (!ac) {
	    if (s_cfg.getSection(username)) {
		msg.setParam("error","offline");
		break;
	    }
	    ObjList* o = s_expand.find(username);
	    if (!o)
		break;
	    ExpandedUser* eu = static_cast<ExpandedUser*>(o->get());
	    if (!eu)
		break;
	    String d;
	    int count = 0;
	    for (ObjList* ob = eu->skipNull(); ob;ob = ob->skipNext()) {
		String* s = static_cast<String*>(ob->get());
		if (!s)
		    continue;
		NamedList* n = s_accounts.getSection(*s);
		if (!n)
		    continue;
		if (count > 0)
		    d << " ";
		d  << n->getValue("data");
		count ++;
	    }
	    if (count > 1)
		data = "fork ";
	    if (count == 0) {
		msg.setParam("error","offline");
		break;
	    }
	    data << d;
	} else {
	    data = ac->getValue("data");
	    msg.copyParams(*ac,"driver");
	}
	msg.retValue() = data;
	Debug("RegFile",DebugInfo,"Routed '%s' via '%s'",username.c_str(),data.c_str());
	return true;
    }
    return false;
}

static int s_count = 0;

bool ExpireHandler::received(Message &msg)
{
    if ((s_count = (s_count+1) % 30)) // Check for timeouts once at 30 seconds
	return false;
    u_int64_t time = msg.msgTime().sec();
    Lock lock(s_mutex);
    int count = s_accounts.sections();
    for (int i = 0;i < count;) {
	NamedList* sec = s_accounts.getSection(i);
	if (sec && *sec != s_general && expired(*sec,time)) {
	    Debug("RegFile",DebugAll,"Removing user %s, Reson: Registration expired",sec->c_str());
	    s_accounts.clearSection(*sec);
	    count--;
	} else
	   i++;
    }
    if (s_accounts)
	s_accounts.save();
    return false;
}

bool StatusHandler::received(Message &msg)
{
    bool first = true;
    String dest(msg.getValue("module"));
    if (dest && (dest != "regfile") && (dest != "misc"))
	return false;
    Lock lock(s_mutex);
    unsigned int n = s_cfg.sections();
    if (s_cfg.getSection("general") || !s_cfg.getSection(0))
	n--;
    msg.retValue() << "name=regfile,type=misc;create=" << s_create;
    msg.retValue() << ",defined=" << n;
    n = s_accounts.sections();
    if (!s_accounts.getSection(0))
	n--;
    msg.retValue() << ",users=" << n;
    if (msg.getBoolValue("details",true)) {
	msg.retValue() << ";";
	unsigned int count = s_accounts.sections();
	for (unsigned int i = 0; i < count; i ++) {
	    NamedList* ac = s_accounts.getSection(i);
	    if (!ac)
		continue;
	    String data = ac->getValue("data");
	    if (data.null())
		continue;
	    if (first)
		first = false;
	    else
		msg.retValue() << ",";
	    for (char* s = const_cast<char*>(data.c_str()); *s; s++) {
		if (*s < ' ' || *s == ',')
		    *s = '?';
	    }
	    msg.retValue() << *ac << "=" << data;
	}
    }
    msg.retValue() << "\r\n";
    return false;
}

RegfilePlugin::RegfilePlugin()
    : Plugin("regfile"),
      m_init(false)
{
    Output("Loaded module Registration from file");
}

RegfilePlugin::~RegfilePlugin()
{
    Output("Unload module Registration from file");
    if (s_accounts)
	s_accounts.save();
}

void RegfilePlugin::initialize()
{
    Output("Initializing module Register from file");
    Lock lock(s_mutex);
    s_cfg.load();
    if (!m_init) {
	m_init = true;
	s_create = s_cfg.getBoolValue("general","autocreate",false);
	String conf = s_cfg.getValue("general","file");
	if (conf) {
	    s_accounts = conf;
	    s_accounts.load();
	}
	Engine::install(new AuthHandler("user.auth",s_cfg.getIntValue("general","auth",100)));
	Engine::install(new RegistHandler("user.register",s_cfg.getIntValue("general","register",100)));
	Engine::install(new UnRegistHandler("user.unregister",s_cfg.getIntValue("general","register",100)));
	Engine::install(new RouteHandler("call.route",s_cfg.getIntValue("general","route",100)));
	Engine::install(new StatusHandler("engine.status"));
	Engine::install(new ExpireHandler());
    }
    populate();
}

void RegfilePlugin::populate()
{
    s_expand.clear();
    int count = s_cfg.sections();
    for (int i = 0;i < count; i++) {
	NamedList* nl = s_cfg.getSection(i);
	if (!nl || *nl == s_general)
	    continue;
	String* ids = nl->getParam("alternatives");
	if (!ids)
	    continue;
	ObjList* ob = ids->split(',');
	for (ObjList* o = ob->skipNull(); o;o = o->skipNext()) {
	    String* sec = static_cast<String*>(o->get());
	    if (!sec)
		continue;
	    ObjList* ret = s_expand.find(*sec);
	    ExpandedUser* eu = 0;
	    if (!ret) {
		eu = new ExpandedUser(*sec);
		s_expand.append(eu);
	    } else
		eu = static_cast<ExpandedUser*>(ret->get());
	    eu->append(new String(*nl));
	}
	TelEngine::destruct(ob);
    }
    if (s_create)
	return;
    count = s_accounts.sections();
    for (int i = 0;i < count;i++) {
	NamedList* nl = s_accounts.getSection(i);
	if (!nl)
	    continue;
	if (s_cfg.getSection(*nl))
	    continue;
	s_accounts.clearSection(*nl);
	count--;
	i--;
    }
}

INIT_PLUGIN(RegfilePlugin);

}; // anonymous namespace

/* vi: set ts=8 sw=4 sts=4 noet: */
