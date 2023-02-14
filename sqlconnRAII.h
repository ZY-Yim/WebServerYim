#ifndef _SQL_CONNEACTION_RAII_
#define _SQL_CONNEACTION_RAII_
#include <mysql/mysql.h>
#include "sqlconnpool.h"

class sqlconnRAII{
public:
	sqlconnRAII(MYSQL **conn, sqlconnpool *connPool);
	~sqlconnRAII();
	
private:
	MYSQL *connRAII;
	sqlconnpool *connpoolRAII;
};

#endif