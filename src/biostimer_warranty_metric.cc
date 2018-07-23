/*  =========================================================================
    warranty_metric - Agent sending metrics about warranty expiration

    Copyright (C) 2014 - 2017 Eaton                                        
                                                                           
    This program is free software; you can redistribute it and/or modify   
    it under the terms of the GNU General Public License as published by   
    the Free Software Foundation; either version 2 of the License, or      
    (at your option) any later version.                                    
                                                                           
    This program is distributed in the hope that it will be useful,        
    but WITHOUT ANY WARRANTY; without even the implied warranty of         
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the          
    GNU General Public License for more details.                           
                                                                           
    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.            
    =========================================================================
*/

/*
@header
    warranty_metric - Agent sending metrics about warranty expiration
@discuss
@end
*/

//#include "fty_expiration_classes.h"
#include <functional>
#include <malamute.h>
#include <fty_log.h>
#include <fty_proto.h>
#include <tntdb.h>

#define NAME "warranty-metric"
#define LOG_CONFIG "/etc/fty/ftylog.cfg"

uint32_t TTL = 24*60*60;//[s]
const char* MLM_ENDPOINT = "ipc://@/malamute";
std::string url = std::string("mysql:db=box_utf8;user=") +
    ((getenv("DB_USER")   == NULL) ? "root" : getenv("DB_USER")) +
    ((getenv("DB_PASSWD") == NULL) ? ""     :   
     std::string(";password=") + getenv("DB_PASSWD"));


int
select_asset_element_all_with_warranty_end(
        tntdb::Connection& conn,
        std::function<void(
            const tntdb::Row&
            )>& cb)
{
//    LOG_START;

    try{ 
        tntdb::Statement st = conn.prepareCached(
                " SELECT "
                "   v.name as name, t.keytag as keytag, t.value as date "
                " FROM v_web_element v "
                " JOIN t_bios_asset_ext_attributes t "
                " ON "
                "   v.id = t.id_asset_element "
                " WHERE "
                "   t.keytag='end_warranty_date' "
                );

        tntdb::Result res = st.select();

        for (const auto& r: res) {
            cb(r);
        }
//        LOG_END;
        return 0;
    }    
    catch (const std::exception &e) {
//        LOG_END_ABNORMAL(e);
        return -1;
    }    
}

/*
 * Tool will send following messages on the stream METRICS
 *
 *  SUBJECT: end_warranty_date@device
 *           value now() - end_warranty_date
 */
int main (int argc, char *argv [])
{
    ManageFtyLog::setInstanceFtylog(NAME, LOG_CONFIG);
    mlm_client_t *client = mlm_client_new (); 
    assert (client);

    std::function<void(const tntdb::Row&)> cb = \
        [client](const tntdb::Row &row)
        {
            std::string name;
            row["name"].get(name);

            std::string keytag;
            row["keytag"].get(keytag);

            std::string date;
            row["date"].get(date);

            int day_diff;
            {
                struct tm tm_ewd;
                ::memset (&tm_ewd, 0, sizeof(struct tm));

                char* ret = ::strptime (date.c_str(), "%Y-%m-%d", &tm_ewd);
                if (ret == NULL) {
                    log_error ("Cannot convert %s to date, skipping", date.c_str());
                    return;
                }

                time_t ewd = ::mktime (&tm_ewd);
                time_t now = ::time (NULL);
                struct tm *tm_now_p;
                tm_now_p = ::gmtime (&now);
                tm_now_p->tm_hour = 0;
                tm_now_p->tm_min = 0; 
                tm_now_p->tm_sec = 0;
                now = ::mktime (tm_now_p);

                // end_warranty_date (s) - now (s) -> to days
                day_diff = std::ceil ((ewd - now) / (60*60*24));
                log_debug ("day_diff: %d", day_diff);
            }
            log_debug ("name: %s, keytag: %s, date: %s", name.c_str(), keytag.c_str(), date.c_str());
            zmsg_t *msg = fty_proto_encode_metric (
                    NULL,
                    ::time (NULL),
                    3 * TTL,
                    keytag.c_str(),
                    name.c_str (),
                    std::to_string (day_diff).c_str(),
                    "day");
            assert (msg);
            std::string subject = keytag.append ("@").append (name);
            mlm_client_send (client, subject.c_str (), &msg);
        };          

    int r = mlm_client_connect (client, MLM_ENDPOINT, 1000, NAME);
    if (r == -1) { 
        log_error ("Can't connect to malamute");
        exit (EXIT_FAILURE);
    }

    r = mlm_client_set_producer (client, "METRICS");
    if (r == -1) { 
        log_error ("Can't set producer to METRICS stream");
        exit (EXIT_FAILURE);
    }

    // unchecked errors with connection, the tool will fail otherwise
    tntdb::Connection conn = tntdb::connectCached(url);
    r = select_asset_element_all_with_warranty_end (conn, cb);
    if (r == -1) {
        log_error ("Error in element selection");
        exit (EXIT_FAILURE);
    }

    // to ensure all messages got published
    zclock_sleep (500);
    mlm_client_destroy (&client);

    exit (EXIT_SUCCESS);
}
