#include "sqlconnRAII.h"

sqlconnRAII::sqlconnRAII(MYSQL** sql, sqlconnpool* connpool){
    *sql = connpool->get_connection();
    connRAII = *sql;
    connpoolRAII = connpool;
}

sqlconnRAII::~sqlconnRAII(){
    connpoolRAII->release_connection(connRAII);
}