// Copyright (c) 2000, 2018, Oracle and/or its affiliates. All rights reserved.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2.0, as
// published by the Free Software Foundation.
//
// This program is also distributed with certain software (including
// but not limited to OpenSSL) that is licensed under separate terms,
// as designated in a particular file or component or in included license
// documentation. The authors of MySQL hereby grant you an
// additional permission to link the program and your derivative works
// with the separately licensed software that they have included with
// MySQL.
//
// Without limiting anything contained in the foregoing, this file,
// which is part of MySQL Connector/ODBC, is also subject to the
// Universal FOSS Exception, version 1.0, a copy of which can be found at
// http://oss.oracle.com/licenses/universal-foss-exception.
//
// This program is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
// See the GNU General Public License, version 2.0, for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA

/**
  @file  connect.c
  @brief Connection functions.
*/

#include "driver.h"
#include "installer.h"
#include "stringutil.h"

#include <map>
#include <vector>
#include <sstream>
#include <random>

#ifndef _WIN32
#include <netinet/in.h>
#include <resolv.h>
#else
#include <winsock2.h>
#include <windns.h>
#endif

#ifndef CLIENT_NO_SCHEMA
# define CLIENT_NO_SCHEMA      16
#endif


typedef BOOL (*PromptFunc)(SQLHWND, SQLWCHAR *, SQLUSMALLINT,
                           SQLWCHAR *, SQLSMALLINT, SQLSMALLINT *);

const char *my_os_charset_to_mysql_charset(const char *csname);

/**
  Get the connection flags based on the driver options.

  @param[in]  options  Options flags

  @return Client flags suitable for @c mysql_real_connect().
*/
unsigned long get_client_flags(DataSource *ds)
{
  unsigned long flags= CLIENT_MULTI_RESULTS;

  if (ds->safe || ds->return_matching_rows)
    flags|= CLIENT_FOUND_ROWS;
  if (ds->no_catalog)
    flags|= CLIENT_NO_SCHEMA;
  if (ds->use_compressed_protocol)
    flags|= CLIENT_COMPRESS;
  if (ds->ignore_space_after_function_names)
    flags|= CLIENT_IGNORE_SPACE;
  if (ds->allow_multiple_statements)
    flags|= CLIENT_MULTI_STATEMENTS;
  if (ds->clientinteractive)
    flags|= CLIENT_INTERACTIVE;

  return flags;
}


/**
 If it was specified, set the character set for the connection.

 @param[in]  dbc      Database connection
 @param[in]  charset  Character set name
*/
SQLRETURN myodbc_set_initial_character_set(DBC *dbc, const char *charset)
{
  if (dbc->unicode)
  {
    if (charset && charset[0])
    {
      dbc->ansi_charset_info= get_charset_by_csname(charset,
                                                    MYF(MY_CS_PRIMARY),
                                                    MYF(0));
      if (!dbc->ansi_charset_info)
      {
        char errmsg[NAME_LEN + 32*SYSTEM_CHARSET_MBMAXLEN];
        /* This message should help the user to identify the error */
        sprintf(errmsg, "Wrong character set name %.*s", NAME_LEN, charset);
        set_dbc_error(dbc, "HY000", errmsg, 0);

        return SQL_ERROR;
      }
    }

    charset= "utf8";
  }

  if (charset && charset[0])
  {
    if (mysql_set_character_set(dbc->mysql, charset))
    {
      set_dbc_error(dbc, "HY000", mysql_error(dbc->mysql),
                    mysql_errno(dbc->mysql));
      return SQL_ERROR;
    }
  }
  else
  {
    if (mysql_set_character_set(dbc->mysql, dbc->ansi_charset_info->csname))
    {
      set_dbc_error(dbc, "HY000", mysql_error(dbc->mysql),
                    mysql_errno(dbc->mysql));
      return SQL_ERROR;
    }
  }

  {
    MY_CHARSET_INFO my_charset;
    mysql_get_character_set_info(dbc->mysql, &my_charset);
    dbc->cxn_charset_info= get_charset(my_charset.number, MYF(0));
  }

  if (!dbc->unicode)
    dbc->ansi_charset_info= dbc->cxn_charset_info;

  /*
    We always set character_set_results to NULL so we can do our own
    conversion to the ANSI character set or Unicode.
  */
  if (is_minimum_version(dbc->mysql->server_version, "4.1.1")
      && odbc_stmt(dbc, "SET character_set_results = NULL", SQL_NTS, TRUE) != SQL_SUCCESS)
  {
    return SQL_ERROR;
  }

  return SQL_SUCCESS;
}

/*
  Retrieve DNS+SRV list.

  @param[in]  hostname  Full DNS+SRV address (ex: _mysql._tcp.example.com)
  @param[out]  total_weight   Retrieve sum of total weight.

  @return multimap containing list of SRV records
*/


struct Prio
{
  uint16_t prio;
  uint16_t weight;
  operator uint16_t() const
  {
    return prio;
  }

  bool operator < (const Prio &other) const
  {
    return prio < other.prio;
  }
};

struct Srv_host_detail
{
  std::string name;
  unsigned int port = MYSQL_PORT;
};

#ifdef _WIN32
std::multimap<Prio,Srv_host_detail> srv_list(const std::string &hostname,
                                             uint16_t &total_weight)
{
  DNS_STATUS status;               //Return value of  DnsQuery_A() function.
  PDNS_RECORD pDnsRecord =nullptr;          //Pointer to DNS_RECORD structure.

  std::multimap<Prio,Srv_host_detail> srv;

  status = DnsQuery(hostname.c_str(), DNS_TYPE_SRV, DNS_QUERY_STANDARD, nullptr, &pDnsRecord, nullptr);
  if (!status)
  {
    PDNS_RECORD pRecord = pDnsRecord;
    while (pRecord)
    {
      if (pRecord->wType == DNS_TYPE_SRV)
      {
        srv.insert(std::make_pair(
                     Prio({pRecord->Data.Srv.wPriority,
                           pRecord->Data.Srv.wWeight}
                          ),
                     Srv_host_detail
                     {
                       pRecord->Data.Srv.pNameTarget,
                       pRecord->Data.Srv.wPort
                     }));
        total_weight+=pRecord->Data.Srv.wWeight;
      }
      pRecord = pRecord->pNext;
    }

    DnsRecordListFree(pDnsRecord, DnsFreeRecordListDeep);
  }
  return srv;
}
#else

std::multimap<Prio,Srv_host_detail> srv_list(const std::string &hostname,
                                             uint16_t &total_weight)
{
  struct __res_state state {};
  res_ninit(&state);

  std::multimap<Prio,Srv_host_detail> srv;

  unsigned char query_buffer[NS_PACKETSZ];


  //let get
  int res = res_nsearch(&state, hostname.c_str(), ns_c_in, ns_t_srv, query_buffer, sizeof (query_buffer) );

  if (res >= 0)
  {
    ns_msg msg;
    char name_buffer[NS_MAXDNAME];

    ns_initparse(query_buffer, res, &msg);


    auto process = [&msg, &name_buffer, &total_weight, &srv](const ns_rr &rr) -> void
    {
      const unsigned char* srv_data = ns_rr_rdata(rr);
      Srv_host_detail host_data;
      uint16_t prio, weight;

      //Each NS_GET16 call moves srv_data to next value
      NS_GET16(prio, srv_data);
      NS_GET16(weight, srv_data);
      NS_GET16(host_data.port, srv_data);

      dn_expand(ns_msg_base(msg), ns_msg_end(msg),
                srv_data, name_buffer, sizeof(name_buffer));

      host_data.name = name_buffer;

      srv.insert(std::make_pair(Prio({prio, weight}), std::move(host_data)));

      total_weight+=weight;
    };

    for(int x= 0; x < ns_msg_count(msg, ns_s_an); x++)
    {
          ns_rr rr;
          ns_parserr(&msg, ns_s_an, x, &rr);
          process(rr);
    }
  }
  res_nclose(&state);

  return srv;
}
#endif


/*
  Parse a comma separated list of hosts, each optionally specifying a port after
  a colon. If port is not specified then the default port is used.
*/
std::vector<Srv_host_detail> parse_host_list(const char* hosts_str,
                                             unsigned int default_port)
{
  std::vector<Srv_host_detail> list;

  //if hosts_str = nullprt will still add empty host, meaning it will use socket
  std::string hosts(hosts_str ? hosts_str : "");

  size_t pos_i = 0;
  size_t pos_f = std::string::npos;

  do
  {
    pos_f = hosts.find_first_of(",:", pos_i);

    Srv_host_detail host_detail;
    host_detail.name = hosts.substr(pos_i, pos_f-pos_i);

    if( pos_f != std::string::npos && hosts[pos_f] == ':')
    {
      //port
      pos_i = pos_f+1;
      pos_f = hosts.find_first_of(',', pos_i);
      std::string tmp_port = hosts.substr(pos_i,pos_f-pos_i);
      long int val = std::atol(tmp_port.c_str());

      /*
        Note: strtol() returns 0 either if the number is 0
        or conversion was not possible. We distinguish two cases
        by cheking if end pointer was updated.
      */

      if ((val == 0 && tmp_port.length()==0) ||
          (val > 65535 || val < 0))
      {
        std::stringstream err;
        err << "Invalid port value in " << hosts;
        throw err.str();
      }

      host_detail.port = static_cast<uint16_t>(val);
    }
    else
    {
      host_detail.port = default_port;
    }

    pos_i = pos_f+1;

    list.push_back(host_detail);
  }while (pos_f < hosts.size());

  return list;
}


class dbc_guard
{
  DBC *m_dbc;
  bool m_success = false;
  public:

  dbc_guard(DBC *dbc) : m_dbc(dbc) {}
  void set_success(bool success) { m_success = success; }
  ~dbc_guard() { if (!m_success) m_dbc->close(); }
};

/**
  Try to establish a connection to a MySQL server based on the data source
  configuration.

  @param[in]  dbc  Database connection
  @param[in]  ds   Data source information

  @return Standard SQLRETURN code. If it is @c SQL_SUCCESS or @c
  SQL_SUCCESS_WITH_INFO, a connection has been established.
*/
SQLRETURN myodbc_do_connect(DBC *dbc, DataSource *ds)
{
  SQLRETURN rc= SQL_SUCCESS;
  MYSQL *mysql;
  unsigned long flags;
  /* Use 'int' and fill all bits to avoid alignment Bug#25920 */
  unsigned int opt_ssl_verify_server_cert = ~0;
  const my_bool on= 1;
  unsigned int on_int = 1;
  unsigned long max_long = ~0L;

  dbc_guard guard(dbc);

#ifdef WIN32
  /*
   Detect if we are running with ADO present, and force on the
   FLAG_COLUMN_SIZE_S32 option if we are.
  */
  if (GetModuleHandle("msado15.dll") != NULL)
    ds->limit_column_size= 1;

  /* Detect another problem specific to MS Access */
  if (GetModuleHandle("msaccess.exe") != NULL)
    ds->default_bigint_bind_str= 1;
#endif

  mysql = mysql_init(nullptr);
  dbc->mysql = mysql;

  flags= get_client_flags(ds);

  /* Set other connection options */

  if (ds->allow_big_results || ds->safe)
#if MYSQL_VERSION_ID >= 50709
    mysql_options(mysql, MYSQL_OPT_MAX_ALLOWED_PACKET, &max_long);
#else
    /* max_allowed_packet is a magical mysql macro. */
    max_allowed_packet= ~0L;
#endif

  if (ds->force_use_of_named_pipes)
    mysql_options(mysql, MYSQL_OPT_NAMED_PIPE, NullS);

  if (ds->read_options_from_mycnf)
    mysql_options(mysql, MYSQL_READ_DEFAULT_GROUP, "odbc");

  if (ds->initstmt && ds->initstmt[0])
  {
    /* Check for SET NAMES */
    if (is_set_names_statement(ds_get_utf8attr(ds->initstmt,
                                              &ds->initstmt8)))
    {
      return set_dbc_error(dbc, "HY000",
                           "SET NAMES not allowed by driver", 0);
    }
    mysql_options(mysql, MYSQL_INIT_COMMAND, ds->initstmt8);
  }

  if (dbc->login_timeout)
    mysql_options(mysql, MYSQL_OPT_CONNECT_TIMEOUT,
                  (char *)&dbc->login_timeout);

  if (ds->readtimeout)
    mysql_options(mysql, MYSQL_OPT_READ_TIMEOUT,
                  (const char *) &ds->readtimeout);

  if (ds->writetimeout)
    mysql_options(mysql, MYSQL_OPT_WRITE_TIMEOUT,
                  (const char *) &ds->writetimeout);

/*
  Pluggable authentication was introduced in mysql 5.5.7
*/
#if MYSQL_VERSION_ID >= 50507
  if (ds->plugin_dir)
  {
    mysql_options(mysql, MYSQL_PLUGIN_DIR,
                  ds_get_utf8attr(ds->plugin_dir, &ds->plugin_dir8));
  }

  if (ds->default_auth)
  {
    mysql_options(mysql, MYSQL_DEFAULT_AUTH,
                  ds_get_utf8attr(ds->default_auth, &ds->default_auth8));
  }
#endif

  /* set SSL parameters */
  mysql_ssl_set(mysql,
                ds_get_utf8attr(ds->sslkey,    &ds->sslkey8),
                ds_get_utf8attr(ds->sslcert,   &ds->sslcert8),
                ds_get_utf8attr(ds->sslca,     &ds->sslca8),
                ds_get_utf8attr(ds->sslcapath, &ds->sslcapath8),
                ds_get_utf8attr(ds->sslcipher, &ds->sslcipher8));

#if MYSQL_VERSION_ID < 80003
  if (ds->sslverify)
    mysql_options(mysql, MYSQL_OPT_SSL_VERIFY_SERVER_CERT,
                  (const char *)&opt_ssl_verify_server_cert);
#endif

#if MYSQL_VERSION_ID >= 50660
  if (ds->rsakey)
  {
    /* Read the public key on the client side */
    mysql_options(mysql, MYSQL_SERVER_PUBLIC_KEY,
                  ds_get_utf8attr(ds->rsakey, &ds->rsakey8));
  }
#endif
#if MYSQL_VERSION_ID >= 50710
  {
    char tls_options[128] = { 0 };
    if (!ds->no_tls_1)
    {
      strcat(tls_options, "TLSv1");
    }
    if (!ds->no_tls_1_1)
    {
      strcat(tls_options, ds->no_tls_1 ? "TLSv1.1" : ",TLSv1.1");
    }
    if (!ds->no_tls_1_2)
    {
      strcat(tls_options, ds->no_tls_1 && ds->no_tls_1_1 ? "TLSv1.2" : ",TLSv1.2");
    }
    if (tls_options[0])
      mysql_options(mysql, MYSQL_OPT_TLS_VERSION, tls_options);
  }
#endif

#if MYSQL_VERSION_ID >= 80004
  if (ds->get_server_public_key)
  {
    /* Get the server public key */
    mysql_options(mysql, MYSQL_OPT_GET_SERVER_PUBLIC_KEY, (const void*)&on);
  }
#endif

  if (dbc->unicode)
  {
    /*
      Get the ANSI charset info before we change connection to UTF-8.
    */
    MY_CHARSET_INFO my_charset;
    mysql_get_character_set_info(dbc->mysql, &my_charset);
    dbc->ansi_charset_info= get_charset(my_charset.number, MYF(0));
    /*
      We always use utf8 for the connection, and change it afterwards if needed.
    */
    mysql_options(mysql, MYSQL_SET_CHARSET_NAME, "utf8");
    dbc->cxn_charset_info= utf8_charset_info;
  }
  else
  {
#ifdef _WIN32
    char cpbuf[64];
    const char *client_cs_name= NULL;

    myodbc_snprintf(cpbuf, sizeof(cpbuf), "cp%u", GetACP());
    client_cs_name= my_os_charset_to_mysql_charset(cpbuf);

    if (client_cs_name)
    {
      mysql_options(mysql, MYSQL_SET_CHARSET_NAME, client_cs_name);
      dbc->ansi_charset_info= dbc->cxn_charset_info= get_charset_by_csname(client_cs_name, MYF(MY_CS_PRIMARY), MYF(0));
    }
#else
    MY_CHARSET_INFO my_charset;
    mysql_get_character_set_info(dbc->mysql, &my_charset);
    dbc->ansi_charset_info= get_charset(my_charset.number, MYF(0));
#endif
}

#if MYSQL_VERSION_ID >= 50610
  if (ds->can_handle_exp_pwd)
  {
    mysql_options(mysql, MYSQL_OPT_CAN_HANDLE_EXPIRED_PASSWORDS, (char *)&on);
  }
#endif

#if (MYSQL_VERSION_ID >= 50527 && MYSQL_VERSION_ID < 50600) || MYSQL_VERSION_ID >= 50607
  if (ds->enable_cleartext_plugin)
  {
    mysql_options(mysql, MYSQL_ENABLE_CLEARTEXT_PLUGIN, (char *)&on);
  }
#endif

  if (ds->enable_local_infile)
  {
    mysql_options(mysql, MYSQL_OPT_LOCAL_INFILE, &on_int);
  }

  if (ds->load_data_local_dir && ds->load_data_local_dir[0])
  {
    ds_get_utf8attr(ds->load_data_local_dir, &ds->load_data_local_dir8);
    mysql_options(mysql, MYSQL_OPT_LOAD_DATA_LOCAL_DIR,
                  ds->load_data_local_dir8);
  }

#if MYSQL_VERSION_ID >= 50711
  if (ds->sslmode)
  {
    unsigned int mode = 0;
    ds_get_utf8attr(ds->sslmode, &ds->sslmode8);
    if (!myodbc_strcasecmp(ODBC_SSL_MODE_DISABLED, (const char*)ds->sslmode8))
      mode = SSL_MODE_DISABLED;
    if (!myodbc_strcasecmp(ODBC_SSL_MODE_PREFERRED, (const char*)ds->sslmode8))
      mode = SSL_MODE_PREFERRED;
    if (!myodbc_strcasecmp(ODBC_SSL_MODE_REQUIRED, (const char*)ds->sslmode8))
      mode = SSL_MODE_REQUIRED;
    if (!myodbc_strcasecmp(ODBC_SSL_MODE_VERIFY_CA, (const char*)ds->sslmode8))
      mode = SSL_MODE_VERIFY_CA;
    if (!myodbc_strcasecmp(ODBC_SSL_MODE_VERIFY_IDENTITY, (const char*)ds->sslmode8))
      mode = SSL_MODE_VERIFY_IDENTITY;

    // Don't do anything if there is no match with any of the available modes
    if (mode)
      mysql_options(mysql, MYSQL_OPT_SSL_MODE, &mode);
  }
#endif

  std::multimap<Prio,Srv_host_detail> hosts_prio;

  uint16_t total_weight = 0;

  std::vector<Srv_host_detail> hosts;
  try {
    hosts = parse_host_list(ds_get_utf8attr(ds->server, &ds->server8), ds->port);

  } catch (std::string &e)
  {
    set_dbc_error(dbc, "HY000", e.c_str(), 0);
    return SQL_ERROR;
  }

  if(!ds->multi_host && hosts.size() > 1)
  {
    set_dbc_error(dbc, "HY000", "Missing option MULTI_HOST=1", 0);
    return SQL_ERROR;
  }

  if(ds->enable_dns_srv && hosts.size() > 1)
  {
    set_dbc_error(dbc, "HY000", "Specifying multiple hostnames with DNS SRV look up is not allowed.", 0);
    return SQL_ERROR;
  }

  if(ds->enable_dns_srv && ds->has_port)
  {
    set_dbc_error(dbc, "HY000", "Specifying a port number with DNS SRV lookup is not allowed.", 0);
    return SQL_ERROR;
  }

  if(ds->enable_dns_srv)
  {
    if(ds->socket)
    {
      set_dbc_error(dbc, "HY000",
#ifdef _WIN32
                    "Using Named Pipes with DNS SRV lookup is not allowed.",
#else
                    "Using Unix domain sockets with DNS SRV lookup is not allowed",
#endif
                    0);
      return SQL_ERROR;
    }

    hosts_prio = srv_list(hosts[0].name, total_weight);

    if(hosts_prio.empty())
    {
      std::stringstream err;
      err << "Unable to locate any hosts for " << ds->server8;
      set_dbc_error(dbc, "HY000",
                    err.str().c_str(),
                    0);
      return SQL_ERROR;
    }

  }
  else
  {
    for(auto host : hosts)
    {
      hosts_prio.insert(std::make_pair(Prio({0, 0}), host));
    }
  }

  auto do_connect = [&mysql,&ds,&flags,&dbc](
                    const char *host,
                    unsigned int port
                    ) -> short
  {
    int protocol;
    if(ds->socket)
    {
#ifdef _WIN32
      protocol = MYSQL_PROTOCOL_PIPE;
#else
      protocol = MYSQL_PROTOCOL_SOCKET;
#endif
    } else
    {
      protocol = MYSQL_PROTOCOL_TCP;
    }
    mysql_options(mysql, MYSQL_OPT_PROTOCOL, &protocol);

    //Setting server and port to the ds->server8 and ds->port
    //TODO: IS THERE A FUNCTION TO DO THIS USING myodbc_malloc?
    //COPY OF sqlwchardup
    size_t chars= strlen(host);
    x_free(ds->server8);
    ds->server8= (SQLCHAR *)myodbc_malloc(( chars + 1) , MYF(0));
    memcpy(ds->server8, host, chars);
    ds->port = port;

    if (!mysql_real_connect(mysql,
                            host,
                            ds_get_utf8attr(ds->uid,      &ds->uid8),
                            ds_get_utf8attr(ds->pwd,      &ds->pwd8),
                            ds_get_utf8attr(ds->database, &ds->database8),
                            port,
                            ds_get_utf8attr(ds->socket,   &ds->socket8),
                            flags))
    {
      unsigned int native_error= mysql_errno(mysql);

      /* Before 5.6.11 error returned by server was ER_MUST_CHANGE_PASSWORD(1820).
       In 5.6.11 it changed to ER_MUST_CHANGE_PASSWORD_LOGIN(1862)
       We must to change error for old servers in order to set correct sqlstate */
      if (native_error == 1820 && ER_MUST_CHANGE_PASSWORD_LOGIN != 1820)
      {
        native_error= ER_MUST_CHANGE_PASSWORD_LOGIN;
      }

#if MYSQL_VERSION_ID < 50610
      /* In that special case when the driver was linked against old version of libmysql*/
      if (native_error == ER_MUST_CHANGE_PASSWORD_LOGIN
          && ds->can_handle_exp_pwd)
      {
        /* The password has expired, application said it knows how to deal with
         that, but the driver was linked  that
         does not support this option. Thus we change native error. */
        /* TODO: enum/defines for driver specific errors */
        return set_conn_error(dbc, MYERR_08004,
                              "Your password has expired, but underlying library doesn't support "
                              "this functionlaity", 0);
      }
#endif
      set_dbc_error(dbc, "HY000", mysql_error(mysql), native_error);

      translate_error(dbc->error.sqlstate, MYERR_S1000, native_error);

      return SQL_ERROR;
    }

    return SQL_SUCCESS;
  };

  //Connect loop
  {
    bool connected = false;
    std::random_device generator;

    while(!hosts_prio.empty() && !connected)
    {
      auto same_range = hosts_prio.equal_range(hosts_prio.begin()->first);
      std::vector<Srv_host_detail*> same_prio;
      std::vector<uint16_t> weights;

      for(auto it = same_range.first; it != same_range.second; ++it)
      {
        //if weight is not used, we should put all weights = 1 so that
        //discrete_distribution works as expected
        weights.push_back(total_weight!= 0 ? it->first.weight : 1);
        same_prio.push_back(&it->second);
      }

      while (!weights.empty() && !connected)
      {
        std::discrete_distribution<int> distribution(
              weights.begin(), weights.end());

        auto el = same_prio.begin();
        auto weight_el = weights.begin();

        if (same_prio.size() > 1)
        {
          int pos = distribution(generator);
          std::advance(el, pos);
          std::advance(weight_el, pos);
        }

        if(do_connect((*el)->name.c_str(), (*el)->port) == SQL_SUCCESS)
        {
          connected = true;
        }
        else
        {
          switch (mysql_errno(mysql))
          {
          case ER_CON_COUNT_ERROR:
          case CR_SOCKET_CREATE_ERROR:
          case CR_CONNECTION_ERROR:
          case CR_CONN_HOST_ERROR:
          case CR_IPSOCK_ERROR:
          case CR_UNKNOWN_HOST:
            //On Network errors, continue
            break;
          default:
            //If SQLSTATE not 08xxx, which is used for network errors
            if(strncmp(mysql_sqlstate(mysql), "08", 2) != 0)
            {
              //Return error and do not try another host
              return SQL_ERROR;
            }
          }

        }

        same_prio.erase(el);
        weights.erase(weight_el);
      }

      hosts_prio.erase(same_range.first, same_range.second);

    };

    if(!connected)
    {
      std::stringstream err;
      if(ds->enable_dns_srv)
      {
        err << "Unable to connect to any of the hosts of "
            << ds->server8 << " SRV";
        set_dbc_error(dbc, "HY000",
                      err.str().c_str(),
                      0);
      }
      else if (ds->multi_host && hosts.size() > 1) {
        err << "Unable to connect to any of the hosts";
        set_dbc_error(dbc, "HY000",
                      err.str().c_str(),
                      0);
      }
      //The others will retrieve the error from connect
      return SQL_ERROR;
    }
  }

  if (!is_minimum_version(dbc->mysql->server_version, "4.1.1"))
  {
    dbc->close();
    set_dbc_error(dbc, "08001", "Driver does not support server versions under 4.1.1", 0);
    return SQL_ERROR;
  }

  rc= myodbc_set_initial_character_set(dbc, ds_get_utf8attr(ds->charset,
                                                            &ds->charset8));
  if (!SQL_SUCCEEDED(rc))
  {
    /** @todo set failure reason */
    goto error;
  }

  /*
    The MySQL server has a workaround for old versions of Microsoft Access
    (and possibly other products) that is no longer necessary, but is
    unfortunately enabled by default. We have to turn it off, or it causes
    other problems.
  */
  if (!ds->auto_increment_null_search &&
      odbc_stmt(dbc, "SET SQL_AUTO_IS_NULL = 0", SQL_NTS, TRUE) != SQL_SUCCESS)
  {
    /** @todo set error reason */
    goto error;
  }

  dbc->ds= ds;
  /* init all needed UTF-8 strings */
  ds_get_utf8attr(ds->name, &ds->name8);
  ds_get_utf8attr(ds->server, &ds->server8);
  ds_get_utf8attr(ds->uid, &ds->uid8);
  ds_get_utf8attr(ds->pwd, &ds->pwd8);
  ds_get_utf8attr(ds->socket, &ds->socket8);
  if (ds->database)
  {

    dbc->database= ds_get_utf8attr(ds->database, &ds->database8);
  }

  if (ds->save_queries && !dbc->query_log)
    dbc->query_log= init_query_log();

  /* Set the statement error prefix based on the server version. */
  strxmov(dbc->st_error_prefix, MYODBC_ERROR_PREFIX, "[mysqld-",
          mysql->server_version, "]", NullS);

  /* This needs to be set after connection, or it doesn't stick.  */
  if (ds->auto_reconnect)
  {
    mysql_options(mysql, MYSQL_OPT_RECONNECT, (char *)&on);
  }

  /* Make sure autocommit is set as configured. */
  if (dbc->commit_flag == CHECK_AUTOCOMMIT_OFF)
  {
    if (!trans_supported(dbc) || ds->disable_transactions)
    {
      rc= SQL_SUCCESS_WITH_INFO;
      dbc->commit_flag= CHECK_AUTOCOMMIT_ON;
      set_conn_error((DBC*)dbc, MYERR_01S02,
                     "Transactions are not enabled, option value "
                     "SQL_AUTOCOMMIT_OFF changed to SQL_AUTOCOMMIT_ON", 0);
    }
    else if (autocommit_on(dbc) && mysql_autocommit(mysql, FALSE))
    {
      /** @todo set error */
      goto error;
    }
  }
  else if ((dbc->commit_flag == CHECK_AUTOCOMMIT_ON) &&
           trans_supported(dbc) && !autocommit_on(dbc))
  {
    if (mysql_autocommit(mysql, TRUE))
    {
      /** @todo set error */
      goto error;
    }
  }

  /* Set transaction isolation as configured. */
  if (dbc->txn_isolation != DEFAULT_TXN_ISOLATION)
  {
    char buff[80];
    const char *level;

    if (dbc->txn_isolation & SQL_TXN_SERIALIZABLE)
      level= "SERIALIZABLE";
    else if (dbc->txn_isolation & SQL_TXN_REPEATABLE_READ)
      level= "REPEATABLE READ";
    else if (dbc->txn_isolation & SQL_TXN_READ_COMMITTED)
      level= "READ COMMITTED";
    else
      level= "READ UNCOMMITTED";

    if (trans_supported(dbc))
    {
      sprintf(buff, "SET SESSION TRANSACTION ISOLATION LEVEL %s", level);
      if (odbc_stmt(dbc, buff, SQL_NTS, TRUE) != SQL_SUCCESS)
      {
        /** @todo set error reason */
        goto error;
      }
    }
    else
    {
      dbc->txn_isolation= SQL_TXN_READ_UNCOMMITTED;
      rc= SQL_SUCCESS_WITH_INFO;
      set_conn_error((DBC*)dbc, MYERR_01S02,
                     "Transactions are not enabled, so transaction isolation "
                     "was ignored.", 0);
    }
  }

#if MYSQL_VERSION_ID >= 50709
  mysql_get_option(mysql, MYSQL_OPT_NET_BUFFER_LENGTH, &dbc->net_buffer_len);
#else
  // for older versions just use net_buffer_length() macro
  dbc->net_buffer_len = net_buffer_length;
#endif
  guard.set_success(rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO);
  return rc;

error:
  return SQL_ERROR;
}


/**
  Establish a connection to a data source.

  @param[in]  hdbc    Connection handle
  @param[in]  szDSN   Data source name (in connection charset)
  @param[in]  cbDSN   Length of data source name in bytes or @c SQL_NTS
  @param[in]  szUID   User identifier (in connection charset)
  @param[in]  cbUID   Length of user identifier in bytes or @c SQL_NTS
  @param[in]  szAuth  Authentication string (password) (in connection charset)
  @param[in]  cbAuth  Length of authentication string in bytes or @c SQL_NTS

  @return  Standard ODBC success codes

  @since ODBC 1.0
  @since ISO SQL 92
*/
SQLRETURN SQL_API MySQLConnect(SQLHDBC   hdbc,
                               SQLWCHAR *szDSN, SQLSMALLINT cbDSN,
                               SQLWCHAR *szUID, SQLSMALLINT cbUID,
                               SQLWCHAR *szAuth, SQLSMALLINT cbAuth)
{
  SQLRETURN rc;
  DBC *dbc= (DBC *)hdbc;
  DataSource *ds;

#ifdef NO_DRIVERMANAGER
  return set_dbc_error((DBC*)dbc, "HY000",
                       "SQLConnect requires DSN and driver manager", 0);
#else

  /* Can't connect if we're already connected. */
  if (is_connected(dbc))
    return set_conn_error((DBC*)hdbc, MYERR_08002, NULL, 0);

  /* Reset error state */
  CLEAR_DBC_ERROR(dbc);

  if (szDSN && !szDSN[0])
  {
    return set_conn_error((DBC*)hdbc, MYERR_S1000,
                          "Invalid connection parameters", 0);
  }

  ds= ds_new();

  ds_set_strnattr(&ds->name, szDSN, cbDSN);
  ds_set_strnattr(&ds->uid, szUID, cbUID);
  ds_set_strnattr(&ds->pwd, szAuth, cbAuth);

  ds_lookup(ds);

  rc= myodbc_do_connect(dbc, ds);

  if (!dbc->ds)
    ds_delete(ds);
  return rc;
#endif
}


/**
  An alternative to SQLConnect that allows specifying more of the connection
  parameters, and whether or not to prompt the user for more information
  using the setup library.

  NOTE: The prompting done in this function using MYODBCUTIL_DATASOURCE has
        been observed to cause an access violation outside of our code
        (specifically in Access after prompting, before table link list).
        This has only been seen when setting a breakpoint on this function.

  @param[in]  hdbc  Handle of database connection
  @param[in]  hwnd  Window handle. May be @c NULL if no prompting will be done.
  @param[in]  szConnStrIn  The connection string
  @param[in]  cbConnStrIn  The length of the connection string in characters
  @param[out] szConnStrOut Pointer to a buffer for the completed connection
                           string. Must be at least 1024 characters.
  @param[in]  cbConnStrOutMax Length of @c szConnStrOut buffer in characters
  @param[out] pcbConnStrOut Pointer to buffer for returning length of
                            completed connection string, in characters
  @param[in]  fDriverCompletion Complete mode, one of @cSQL_DRIVER_PROMPT,
              @c SQL_DRIVER_COMPLETE, @c SQL_DRIVER_COMPLETE_REQUIRED, or
              @cSQL_DRIVER_NOPROMPT

  @return Standard ODBC return codes

  @since ODBC 1.0
  @since ISO SQL 92
*/
SQLRETURN SQL_API MySQLDriverConnect(SQLHDBC hdbc, SQLHWND hwnd,
                                     SQLWCHAR *szConnStrIn,
                                     SQLSMALLINT cbConnStrIn,
                                     SQLWCHAR *szConnStrOut,
                                     SQLSMALLINT cbConnStrOutMax,
                                     SQLSMALLINT *pcbConnStrOut,
                                     SQLUSMALLINT fDriverCompletion)
{
  SQLRETURN rc= SQL_SUCCESS;
  DBC *dbc= (DBC *)hdbc;
  DataSource *ds= ds_new();
  /* We may have to read driver info to find the setup library. */
  Driver *pDriver= driver_new();
  SQLWCHAR *prompt_instr= NULL;
  /* We never know how many new parameters might come out of the prompt */
  SQLWCHAR prompt_outstr[4096];
  size_t prompt_inlen;
  BOOL bPrompt= FALSE;
  HMODULE hModule= NULL;

  if (cbConnStrIn != SQL_NTS)
    szConnStrIn= sqlwchardup(szConnStrIn, cbConnStrIn);

  /* Parse the incoming string */
  if (ds_from_kvpair(ds, szConnStrIn, (SQLWCHAR)';'))
  {
    rc= set_dbc_error(dbc, "HY000",
                      "Failed to parse the incoming connect string.", 0);
    goto error;
  }

#ifndef NO_DRIVERMANAGER
  /*
   If the connection string contains the DSN keyword, the driver retrieves
   the information for the specified data source (and merges it into the
   connection info with the provided connection info having precedence).

   This also allows us to get pszDRIVER (if not already given).
  */
  if (ds->name)
  {
     ds_lookup(ds);

    /*
      If DSN is used:
      1 - we want the connection string options to override DSN options
      2 - no need to check for parsing erros as it was done before
    */
    ds_from_kvpair(ds, szConnStrIn, (SQLWCHAR)';');
  }
#endif

  /* If FLAG_NO_PROMPT is not set, force prompting off. */
  if (ds->dont_prompt_upon_connect)
    fDriverCompletion= SQL_DRIVER_NOPROMPT;

  /*
    We only prompt if we need to.

    A not-so-obvious gray area is when SQL_DRIVER_COMPLETE or
    SQL_DRIVER_COMPLETE_REQUIRED was specified along without a server or
    password - particularly password. These can work with defaults/blank but
    many callers expect prompting when these are blank. So we compromise; we
    try to connect and if it works we say its ok otherwise we go to
    a prompt.
  */
  switch (fDriverCompletion)
  {
  case SQL_DRIVER_PROMPT:
    bPrompt= TRUE;
    break;

  case SQL_DRIVER_COMPLETE:
  case SQL_DRIVER_COMPLETE_REQUIRED:
    if (myodbc_do_connect(dbc, ds) == SQL_SUCCESS)
      goto connected;
    bPrompt= TRUE;
    break;

  case SQL_DRIVER_NOPROMPT:
    bPrompt= FALSE;
    break;

  default:
    rc= set_dbc_error((DBC*)hdbc, "HY110", "Invalid driver completion.", 0);
    goto error;
  }

#ifdef __APPLE__
  /*
    We don't support prompting on Mac OS X.
  */
  if (bPrompt)
  {
    rc= set_dbc_error((DBC*)hdbc, "HY000",
                           "Prompting is not supported on this platform. "
                           "Please provide all required connect information.",
                           0);
    goto error;
  }
#endif

  if (bPrompt)
  {
    PromptFunc pFunc;

    /*
     We can not present a prompt unless we can lookup the name of the setup
     library file name. This means we need a DRIVER. We try to look it up
     using a DSN (above) or, hopefully, get one in the connection string.

     This will, when we need to prompt, trump the ODBC rule where we can
     connect with a DSN which does not exist. A possible solution would be to
     hard-code some fall-back value for ds->pszDRIVER.
    */
    if (!ds->driver)
    {
      char szError[1024];
      sprintf(szError,
              "Could not determine the driver name; "
              "could not lookup setup library. DSN=(%s)\n",
              ds_get_utf8attr(ds->name, &ds->name8));
      rc= set_dbc_error((DBC*)hdbc, "HY000", szError, 0);
      goto error;
    }

#ifndef NO_DRIVERMANAGER
    /* We can not present a prompt if we have a null window handle. */
    if (!hwnd)
    {
      rc= set_dbc_error((DBC*)hdbc, "IM008", "Invalid window handle", 0);
      goto error;
    }

    /* if given a named DSN, we will have the path in the DRIVER field */
    if (ds->name)
        sqlwcharncpy(pDriver->lib, ds->driver, ODBCDRIVER_STRLEN);
    /* otherwise, it's the driver name */
    else
        sqlwcharncpy(pDriver->name, ds->driver, ODBCDRIVER_STRLEN);

    if (driver_lookup(pDriver))
    {
      char sz[1024];
      sprintf(sz, "Could not find driver '%s' in system information.",
              ds_get_utf8attr(ds->driver, &ds->driver8));

      rc= set_dbc_error((DBC*)hdbc, "IM003", sz, 0);
      goto error;
    }

    if (!*pDriver->setup_lib)
#endif
    {
      rc= set_dbc_error((DBC*)hdbc, "HY000",
                        "Could not determine the file name of setup library.",
                        0);
      goto error;
    }

    /*
     We dynamically load the setup library so the driver itself does not
     depend on GUI libraries.
    */
#ifndef WIN32
/*
    lt_dlinit();
*/
#endif

    if (!(hModule= LoadLibrary(ds_get_utf8attr(pDriver->setup_lib,
                                               &pDriver->setup_lib8))))
    {
      char sz[1024];
      sprintf(sz, "Could not load the setup library '%s'.",
              ds_get_utf8attr(pDriver->setup_lib, &pDriver->setup_lib8));
      rc= set_dbc_error((DBC*)hdbc, "HY000", sz, 0);
      goto error;
    }

    pFunc= (PromptFunc)GetProcAddress(hModule, "Driver_Prompt");

    if (pFunc == NULL)
    {
#ifdef WIN32
      LPVOID pszMsg;

      FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
                    NULL, GetLastError(),
                    MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                    (LPTSTR)&pszMsg, 0, NULL);
      rc= set_dbc_error((DBC*)hdbc, "HY000", (char *)pszMsg, 0);
      LocalFree(pszMsg);
#else
      rc= set_dbc_error((DBC*)hdbc, "HY000", dlerror(), 0);
#endif
      goto error;
    }

    /* create a string for prompting, and add driver manually */
    prompt_inlen= ds_to_kvpair_len(ds) + sqlwcharlen(W_DRIVER_PARAM) +
                  sqlwcharlen(ds->driver) + 1;
    prompt_instr= (SQLWCHAR *) myodbc_malloc(prompt_inlen * sizeof(SQLWCHAR),
                                         MYF(0));
    if (ds_to_kvpair(ds, prompt_instr, prompt_inlen, ';') == -1)
    {
      rc= set_dbc_error(dbc, "HY000",
                        "Failed to prepare prompt input.", 0);
      goto error;
    }
    prompt_inlen-= sqlwcharlen(prompt_instr);
    sqlwcharncat2(prompt_instr, W_DRIVER_PARAM, &prompt_inlen);
    sqlwcharncat2(prompt_instr, ds->driver, &prompt_inlen);

    /*
      In case the client app did not provide the out string we use our
      inner buffer prompt_outstr
    */
    if (!pFunc(hwnd, prompt_instr, fDriverCompletion,
               prompt_outstr, sizeof(prompt_outstr), pcbConnStrOut))
    {
      set_dbc_error((DBC*)hdbc, "HY000", "User cancelled.", 0);
      rc= SQL_NO_DATA;
      goto error;
    }

    /* refresh our DataSource */
    ds_delete(ds);
    ds= ds_new();
    if (ds_from_kvpair(ds, prompt_outstr, ';'))
    {
      rc= set_dbc_error(dbc, "HY000",
                        "Failed to parse the prompt output string.", 0);
      goto error;
    }

    /*
      We don't need prompt_outstr after the new DataSource is created.
      Copy its contents into szConnStrOut if possible
    */
    if (szConnStrOut)
    {
      *pcbConnStrOut= (SQLSMALLINT)myodbc_min(cbConnStrOutMax, *pcbConnStrOut);
      memcpy(szConnStrOut, prompt_outstr, (size_t)*pcbConnStrOut*sizeof(SQLWCHAR));
      /* term needed if possibly truncated */
      szConnStrOut[*pcbConnStrOut - 1] = 0;
    }

  }

  if ((rc= myodbc_do_connect(dbc, ds)) != SQL_SUCCESS)
  {
    goto error;
  }

  if (ds->savefile)
  {
    /* We must disconnect if File DSN is created */
    dbc->close();
  }

connected:

  /* copy input to output if connected without prompting */
  if (!bPrompt && szConnStrOut && cbConnStrOutMax)
  {
    size_t inlen, copylen;
    SQLWCHAR *internal_out_str= szConnStrIn;

    if (ds->savefile)
    {
      SQLWCHAR *pwd_temp= ds->pwd;
      SQLCHAR  *pwd8_temp= ds->pwd8;

      /* make sure the password does not go into the output buffer */
      ds->pwd= NULL;
      ds->pwd8= NULL;

      ds_to_kvpair(ds, prompt_outstr, sizeof(prompt_outstr)/sizeof(SQLWCHAR), ';');
      internal_out_str= (SQLWCHAR*)prompt_outstr;

      /* restore old values */
      ds->pwd= pwd_temp;
      ds->pwd8= pwd8_temp;
    }

    inlen= (sqlwcharlen(internal_out_str) + 1) * sizeof(SQLWCHAR);
    copylen= myodbc_min((size_t)cbConnStrOutMax, inlen);
    memcpy(szConnStrOut, internal_out_str, copylen);
    /* term needed if possibly truncated */
    szConnStrOut[(copylen / sizeof(SQLWCHAR)) - 1] = 0;
    if (pcbConnStrOut)
      *pcbConnStrOut= (copylen / sizeof(SQLWCHAR)) - 1;
  }

  /* return SQL_SUCCESS_WITH_INFO if truncated output string */
  if (pcbConnStrOut &&
      cbConnStrOutMax - sizeof(SQLWCHAR) == *pcbConnStrOut * sizeof(SQLWCHAR))
  {
    set_dbc_error((DBC*)hdbc, "01004", "String data, right truncated.", 0);
    rc= SQL_SUCCESS_WITH_INFO;
  }

error:
  if (hModule)
    FreeLibrary(hModule);
  if (cbConnStrIn != SQL_NTS)
    x_free(szConnStrIn);

  driver_delete(pDriver);
  /* delete data source unless connected */
  if (!dbc->ds)
    ds_delete(ds);
  x_free(prompt_instr);

  return rc;
}


void DBC::free_connection_stmts()
{
  for (auto it = stmt_list.begin(); it != stmt_list.end(); )
  {
      STMT *stmt = *it;
      it = stmt_list.erase(it);
      my_SQLFreeStmt((SQLHSTMT)stmt, SQL_DROP);
  }
  stmt_list.clear();
}


/**
  Disconnect a connection.

  @param[in]  hdbc   Connection handle

  @return  Standard ODBC return codes

  @since ODBC 1.0
  @since ISO SQL 92
*/
SQLRETURN SQL_API SQLDisconnect(SQLHDBC hdbc)
{
  DBC *dbc= (DBC *) hdbc;

  CHECK_HANDLE(hdbc);

  dbc->free_connection_stmts();

  dbc->close();

  if (dbc->ds && dbc->ds->save_queries)
    end_query_log(dbc->query_log);

  /* free allocated packet buffer */

  if(dbc->ds)
  {
    ds_delete(dbc->ds);
  }
  dbc->ds= NULL;
  dbc->database.clear();

  return SQL_SUCCESS;
}

