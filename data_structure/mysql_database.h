#include <mysqlx/xdevapi.h>

#include <memory>
#pragma once

using namespace std;

namespace slog {

// template <typename KeyType, typename ValueType>
// class MySQLClient
// {
// private:
//     /* data */
//     Session sess;
//     Schema db;

// public:
//     MySQLClient(/* args */);
//     ~MySQLClient();
// };

// MySQLClient::MySQLClient(/* args */)
// {
// }

// MySQLClient::~MySQLClient()
// {
// }

template <typename KeyType, typename ValueType, typename HashFn = std::hash<KeyType>, uint8_t ShardBits = 8>
// don't know why need to add typename HashFn and uint8_t ShardBits = 8
class MySQLDatabase {
 private:
  /* data */
  string mysql_host;
  int mysql_port;
  string mysql_user;
  string mysql_pwd;
  string mysql_sock;
  // mysqlx::Session* mysql_sess;
  mysqlx::Session* mysql_sess;
  // mysqlx::Schema mysql_sch;

 public:
  MySQLDatabase() {
    // create MySQL connection by default
    mysql_host = "localhost";
    mysql_port = 3750;
    mysql_user = "root";
    mysql_pwd = "ustc1234";
    mysql_sock = "/home/edwardzcn/test/mysql/data/m1/mysql.sock";
    cout << "Creating mysql session on " << mysql_host << ":" << mysql_port << endl;
    // mysql_sess = new mysqlx::Session("localhost",3750,"edwardzcn","ustc1234");
    mysqlx::SessionSettings option(mysql_host, mysql_port, mysql_user, mysql_pwd);
    mysql_sess = new mysqlx::Session(option);
    // !TODO: remove me (for session test)
    cout << "Session accepted, creating coolection from db 'test1'." << endl;
    // mysql_sch = mysql_sess->getSchema("test1");
    mysqlx::Schema mysql_sch = mysql_sess->getSchema("test1");
    // !TODO: remove me (for session db test)
    auto table_names = mysql_sch.getTableNames();
    cout << "Tables in db 'test1' are: " << table_names;

    mysql_sess->sql("use test1");
  }
  ~MySQLDatabase() { delete mysql_sess; }

  bool Get(ValueType& res, const KeyType& key) const {
    bool found = false;
    // no lock
    mysqlx::RowResult rs = mysql_sess->sql("SELECT record FROM kv WHERE key=" + key).execute();
    if (rs.count() != 0) {
      found = true;
      auto it = rs.begin();
      res = (*it).get(0).get<string>();
    }
    return found;
  }

  bool InsertOrUpdate(const KeyType& key, const ValueType& value) {
    // Select then decide to insert or update
    bool key_exists = false;
    mysqlx::RowResult rs = mysql_sess->sql("SELECT record FROM kv WHERE key='" + key + "';").execute();
    if (rs.count() != 0) {
      // If key does not exist;
      // !TODO: Fixme: to_string(value)
      // !TODO: Fixme: the type of rsi and rsu
      auto rsi = mysql_sess->sql("INSERT INTO kv VALUES ('" + key + "','test_value');");
    } else {
      key_exists = true;
      auto rsu = mysql_sess->sql("UPDATE kv SET value='test_value' WHERE key='" + key + "';");
    }
    return key_exists;
  }

  bool Erase(const KeyType& key) {
    bool key_exists = false;
    auto rsd = mysql_sess->sql("DELETE FROM kv WHERE key='" + key + "';");
    // !TODO: Fixme: check rsd's type
    // if (rsd.count() != 0) {
    //   key_exists = true;
    // }
    return key_exists;
  }
};

}  // namespace slog
