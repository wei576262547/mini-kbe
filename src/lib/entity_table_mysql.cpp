/*
This source file is part of KBEngine
For the latest info, see http://www.kbengine.org/

Copyright (c) 2008-2016 KBEngine.

KBEngine is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

KBEngine is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.
 
You should have received a copy of the GNU Lesser General Public License
along with KBEngine.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "entity_table_mysql.h"
#include "kbe_table_mysql.h"
#include "read_entity_helper.h"
#include "write_entity_helper.h"
#include "remove_entity_helper.h"
//#include "entitydef/scriptdef_module.h"
//#include "entitydef/property.h"
#include "db_interface/db_interface.h"
#include "db_interface/entity_table.h"
#include "network/fixed_messages.h"

#ifndef CODE_INLINE
#include "entity_table_mysql.inl"
#endif

namespace KBEngine { 

// ͬ���ɹ�ʱ�ص�
typedef void (*onSyncItemToDBSuccessPtr)(DBInterface*, const char*, const char*);

bool sync_item_to_db(DBInterface* dbi, 
					 const char* datatype, 
					 const char* tableName, 
					 const char* itemName, 
					 uint32 length, 
					 enum_field_types 
					 sqlitemtype, 
					 unsigned int itemflags, 
					 void* pData, onSyncItemToDBSuccessPtr callback = NULL)	
{
	if(pData)
	{
		DBInterfaceMysql::TABLE_FIELDS* pTFData = static_cast<DBInterfaceMysql::TABLE_FIELDS*>(pData);
		DBInterfaceMysql::TABLE_FIELDS::iterator iter = pTFData->find(itemName);
		if(iter != pTFData->end())
		{
			TABLE_FIELD& tf = iter->second;
			if(tf.type == sqlitemtype && ((tf.flags & itemflags) == itemflags))
			{
				if((length == 0) || (int32)length == tf.length)
					return true;
			}
		}
	}

	DEBUG_MSG(fmt::format("syncToDB(): {}->{}({}).\n", tableName, itemName, datatype));

	char __sql_str__[MAX_BUF];

	kbe_snprintf(__sql_str__, MAX_BUF, "ALTER TABLE `" ENTITY_TABLE_PERFIX "_%s` ADD `%s` %s;",
		tableName, itemName, datatype);	

	try
	{
		dbi->query(__sql_str__, strlen(__sql_str__), false);	
	}
	catch(...)
	{
	}	

	if(dbi->getlasterror() == 1060)	
	{
		kbe_snprintf(__sql_str__, MAX_BUF, "ALTER TABLE `" ENTITY_TABLE_PERFIX "_%s` MODIFY COLUMN `%s` %s;",	
			tableName, itemName, datatype);

		try
		{
			if(dbi->query(__sql_str__, strlen(__sql_str__), false))
			{
				if(callback)
					(*callback)(dbi, tableName, itemName);

				return true;
			}
		}
		catch(...)
		{
			ERROR_MSG(fmt::format("syncToDB(): {}->{}({}) is error({}: {})\n lastQuery: {}.\n", 
				tableName, itemName, datatype, dbi->getlasterror(), dbi->getstrerror(), static_cast<DBInterfaceMysql*>(dbi)->lastquery()));

			return false;
		}
	}

	if(callback)
		(*callback)(dbi, tableName, itemName);

	return true;
}		

void sync_autoload_item_index(DBInterface* dbi, const char* tableName, const char* itemName)
{
	// ����sm_autoLoad������
	std::string sql = fmt::format("ALTER TABLE " ENTITY_TABLE_PERFIX "_{} ADD INDEX ({})", tableName, itemName);

	try
	{
		dbi->query(sql.c_str(), sql.size(), true);
	}
	catch(...)
	{
	}
}

//-------------------------------------------------------------------------------------
EntityTableMysql::EntityTableMysql()
{
}

//-------------------------------------------------------------------------------------
EntityTableMysql::~EntityTableMysql()
{
}

//-------------------------------------------------------------------------------------
bool EntityTableMysql::initialize(std::string name)
{
	// ��ȡ����
	tableName(name);

	// �ҵ����д洢���Բ��Ҵ��������е��ֶ�
	std::string hasUnique = "";

	for(; iter != pdescrsMap.end(); ++iter)
	{
		PropertyDescription* pdescrs = iter->second;

		EntityTableItem* pETItem = this->createItem(pdescrs->getDataType()->getName());

		pETItem->pParentTable(this);
		pETItem->utype(pdescrs->getUType());
		pETItem->tableName(this->tableName());

		bool ret = pETItem->initialize(pdescrs, pdescrs->getDataType(), pdescrs->getName());
		
		if(!ret)
		{
			delete pETItem;
			return false;
		}
		
		tableItems_[pETItem->utype()].reset(pETItem);
		tableFixedOrderItems_.push_back(pETItem);
	}
	init_db_item_name();
	return true;
}

//-------------------------------------------------------------------------------------
void EntityTableMysql::init_db_item_name()
{
	EntityTable::TABLEITEM_MAP::iterator iter = tableItems_.begin();
	for(; iter != tableItems_.end(); ++iter)
	{
		// ����fixedDict�ֶ����Ƶ��������
		std::string exstrFlag = "";
		if(iter->second->type() == TABLE_ITEM_TYPE_FIXEDDICT)
		{
			exstrFlag = iter->second->itemName();
			if(exstrFlag.size() > 0)
				exstrFlag += "_";
		}

		static_cast<EntityTableItemMysqlBase*>(iter->second.get())->init_db_item_name(exstrFlag.c_str());
	}
}

//-------------------------------------------------------------------------------------
bool EntityTableMysql::syncIndexToDB(DBInterface* dbi)
{
	std::vector<EntityTableItem*> indexs;

	EntityTable::TABLEITEM_MAP::iterator iter = tableItems_.begin();
	for(; iter != tableItems_.end(); ++iter)
	{
		if(strlen(iter->second->indexType()) == 0)
			continue;

		indexs.push_back(iter->second.get());
	}

	char sql_str[MAX_BUF];

	kbe_snprintf(sql_str, MAX_BUF, "show index from " ENTITY_TABLE_PERFIX "_%s", 
		tableName());

	try
	{
		bool ret = dbi->query(sql_str, strlen(sql_str), false);
		if(!ret)
		{
			return false;
		}
	}
	catch(...)
	{
		return false;
	}

	KBEUnordered_map<std::string, std::string> currDBKeys;

	MYSQL_RES * pResult = mysql_store_result(static_cast<DBInterfaceMysql*>(dbi)->mysql());
	if(pResult)
	{
		MYSQL_ROW arow;
		while((arow = mysql_fetch_row(pResult)) != NULL)
		{
			std::string keytype = "UNIQUE";

			if(std::string("1") == arow[1])
				keytype = "INDEX";

			std::string keyname = arow[2];
			std::string colname = arow[4];

			if(keyname == "PRIMARY" || colname != keyname || 
				keyname == TABLE_PARENTID_CONST_STR ||
				keyname == TABLE_ID_CONST_STR ||
				keyname == TABLE_ITEM_PERFIX"_" TABLE_AUTOLOAD_CONST_STR)
				continue;

			currDBKeys[colname] = keytype;
		}

		mysql_free_result(pResult);
	}

	bool done = false;
	std::string sql = fmt::format("ALTER TABLE " ENTITY_TABLE_PERFIX "_{} ", tableName());
	std::vector<EntityTableItem*>::iterator iiter = indexs.begin();
	for(; iiter != indexs.end(); )
	{
		std::string itemName = fmt::format(TABLE_ITEM_PERFIX"_{}", (*iiter)->itemName());
		KBEUnordered_map<std::string, std::string>::iterator fiter = currDBKeys.find(itemName);
		if(fiter != currDBKeys.end())
		{
			bool deleteIndex = fiter->second != (*iiter)->indexType();
			
			// ɾ���Ѿ������ģ�ʣ�µľ���Ҫ�����ݿ�ɾ����index
			currDBKeys.erase(fiter);
			
			if(deleteIndex)
			{
				sql += fmt::format("DROP INDEX `{}`,", itemName);
				done = true;
			}
			else
			{
				iiter = indexs.erase(iiter);
				continue;
			}
		}

		std::string lengthinfos = "";
		if((*iiter)->type() == TABLE_ITEM_TYPE_BLOB || 
			(*iiter)->type() == TABLE_ITEM_TYPE_STRING ||
			 (*iiter)->type() == TABLE_ITEM_TYPE_UNICODE ||)
		{
			if((*iiter)->pPropertyDescription()->getDatabaseLength() == 0)
			{
				ERROR_MSG(fmt::format("EntityTableMysql::syncIndexToDB(): INDEX({}) without a key length, *.def-><{}>-><DatabaseLength> ? </DatabaseLength>\n", 
					(*iiter)->itemName(), (*iiter)->itemName()));
			}
			else
			{
				lengthinfos = fmt::format("({})", (*iiter)->pPropertyDescription()->getDatabaseLength());
			}
		}

		sql += fmt::format("ADD {} {}({}{}),", (*iiter)->indexType(), itemName, itemName, lengthinfos);
		++iiter;
		done = true;
	}

	// ʣ�µľ���Ҫ�����ݿ�ɾ����index
	KBEUnordered_map<std::string, std::string>::iterator dbkey_iter = currDBKeys.begin();
	for(; dbkey_iter != currDBKeys.end(); ++dbkey_iter)
	{
		sql += fmt::format("DROP INDEX `{}`,", dbkey_iter->first);
		done = true;		
	}
	
	// û����Ҫ�޸Ļ������ӵ�
	if(!done)
		return true;
	
	sql.erase(sql.end() - 1);
	
	try
	{
		bool ret = dbi->query(sql.c_str(), sql.size(), true);
		if(!ret)
		{
			return false;
		}
	}
	catch(...)
	{
		return false;
	}

	return true;
}

//-------------------------------------------------------------------------------------
bool EntityTableMysql::syncToDB(DBInterface* dbi)
{
	if(hasSync())
		return true;

	// DEBUG_MSG(fmt::format("EntityTableMysql::syncToDB(): {}.\n", tableName()));

	char sql_str[MAX_BUF];
	std::string exItems = "";

	if(this->isChild())
		exItems = ", " TABLE_PARENTID_CONST_STR " bigint(20) unsigned NOT NULL, INDEX(" TABLE_PARENTID_CONST_STR ")";

	kbe_snprintf(sql_str, MAX_BUF, "CREATE TABLE IF NOT EXISTS " ENTITY_TABLE_PERFIX "_%s "
			"(id bigint(20) unsigned AUTO_INCREMENT, PRIMARY KEY idKey (id)%s)"
		"ENGINE=" MYSQL_ENGINE_TYPE, 
		tableName(), exItems.c_str());

	try
	{
		bool ret = dbi->query(sql_str, strlen(sql_str), false);
		if(!ret)
		{
			return false;
		}
	}
	catch(...)
	{
		ERROR_MSG(fmt::format("EntityTableMysql::syncToDB(): is error({}: {})\n lastQuery: {}.\n", 
			dbi->getlasterror(), dbi->getstrerror(), static_cast<DBInterfaceMysql*>(dbi)->lastquery()));

		return false;
	}

	DBInterfaceMysql::TABLE_FIELDS outs;
	static_cast<DBInterfaceMysql*>(dbi)->getFields(outs, this->tableName());

	sync_item_to_db(dbi, "tinyint not null DEFAULT 0", this->tableName(), TABLE_ITEM_PERFIX"_" TABLE_AUTOLOAD_CONST_STR, 0, 
			FIELD_TYPE_TINY, NOT_NULL_FLAG, (void*)&outs, &sync_autoload_item_index);

	EntityTable::TABLEITEM_MAP::iterator iter = tableItems_.begin();
	for(; iter != tableItems_.end(); ++iter)
	{
		if(!iter->second->syncToDB(dbi, (void*)&outs))
			return false;
	}

	std::vector<std::string> dbTableItemNames;

	std::string ttablename = ENTITY_TABLE_PERFIX"_";
	ttablename += tableName();

	dbi->getTableItemNames(ttablename.c_str(), dbTableItemNames);

	// ����Ƿ�����Ҫɾ���ı��ֶ�
	std::vector<std::string>::iterator iter0 = dbTableItemNames.begin();
	for(; iter0 != dbTableItemNames.end(); ++iter0)
	{
		std::string tname = (*iter0);
		
		if(tname == TABLE_ID_CONST_STR || 
			tname == TABLE_ITEM_PERFIX"_" TABLE_AUTOLOAD_CONST_STR || 
			tname == TABLE_PARENTID_CONST_STR)
		{
			continue;
		}

		EntityTable::TABLEITEM_MAP::iterator iter = tableItems_.begin();
		bool found = false;

		for(; iter != tableItems_.end(); ++iter)
		{
			if(iter->second->isSameKey(tname))
			{
				found = true;
				break;
			}
		}

		if(!found)
		{
			if(!dbi->dropEntityTableItemFromDB(ttablename.c_str(), tname.c_str()))
				return false;
		}
	}

	// ͬ��������
	if(!syncIndexToDB(dbi))
		return false;

	sync_ = true;
	return true;
}

//-------------------------------------------------------------------------------------
EntityTableItem* EntityTableMysql::createItem(std::string type)
{
	if(type == "INT8")
	{
		return new EntityTableItemMysql_DIGIT(type, "tinyint not null DEFAULT 0", 4, NOT_NULL_FLAG, FIELD_TYPE_TINY);
	}
	else if(type == "INT16")
	{
		return new EntityTableItemMysql_DIGIT(type, "smallint not null DEFAULT 0", 6, NOT_NULL_FLAG, FIELD_TYPE_SHORT);
	}
	else if(type == "INT32")
	{
		return new EntityTableItemMysql_DIGIT(type, "int not null DEFAULT 0", 11, NOT_NULL_FLAG, FIELD_TYPE_LONG);
	}
	else if(type == "INT64")
	{
		return new EntityTableItemMysql_DIGIT(type, "bigint not null DEFAULT 0", 20, NOT_NULL_FLAG, FIELD_TYPE_LONGLONG);
	}
	else if(type == "UINT8")
	{
		return new EntityTableItemMysql_DIGIT(type, "tinyint unsigned not null DEFAULT 0", 3, NOT_NULL_FLAG|UNSIGNED_FLAG, FIELD_TYPE_TINY);
	}
	else if(type == "UINT16")
	{
		return new EntityTableItemMysql_DIGIT(type, "smallint unsigned not null DEFAULT 0", 5, NOT_NULL_FLAG|UNSIGNED_FLAG, FIELD_TYPE_SHORT);
	}
	else if(type == "UINT32")
	{
		return new EntityTableItemMysql_DIGIT(type, "int unsigned not null DEFAULT 0", 10, NOT_NULL_FLAG|UNSIGNED_FLAG, FIELD_TYPE_LONG);
	}
	else if(type == "UINT64")
	{
		return new EntityTableItemMysql_DIGIT(type, "bigint unsigned not null DEFAULT 0", 20, NOT_NULL_FLAG|UNSIGNED_FLAG, FIELD_TYPE_LONGLONG);
	}
	else if(type == "FLOAT")
	{
		return new EntityTableItemMysql_DIGIT(type, "float not null DEFAULT 0", 0, NOT_NULL_FLAG, FIELD_TYPE_FLOAT);
	}
	else if(type == "DOUBLE")
	{
		return new EntityTableItemMysql_DIGIT(type, "double not null DEFAULT 0", 0, NOT_NULL_FLAG, FIELD_TYPE_DOUBLE);
	}
	else if(type == "STRING")
	{
		return new EntityTableItemMysql_STRING("text", 0, 0, FIELD_TYPE_BLOB);
	}
	else if(type == "UNICODE")
	{
		return new EntityTableItemMysql_UNICODE("text", 0, 0, FIELD_TYPE_BLOB);
	}
	else if(type == "BLOB")
	{
		return new EntityTableItemMysql_BLOB("blob", 0, 0, FIELD_TYPE_BLOB);
	}
	else if(type == "ARRAY")
	{
		return new EntityTableItemMysql_ARRAY("", 0, 0, FIELD_TYPE_BLOB);
	}
	else if(type == "FIXED_DICT")
	{
		return new EntityTableItemMysql_FIXED_DICT("", 0, 0, FIELD_TYPE_BLOB);
	}

	KBE_ASSERT(false && "not found type.\n");
	return new EntityTableItemMysql_STRING("", 0, 0, FIELD_TYPE_STRING);
}

//-------------------------------------------------------------------------------------
DBID EntityTableMysql::writeTable(DBInterface* dbi, DBID dbid, int8 shouldAutoLoad, MemoryStream* s)
{
	DBContext context;
	context.parentTableName = "";
	context.parentTableDBID = 0;
	context.dbid = dbid;
	context.tableName = pModule->getName();
	context.isEmpty = false;
	context.readresultIdx = 0;

	while(s->length() > 0)
	{
		ENTITY_PROPERTY_UID pid;
		(*s) >> pid;
		
		EntityTableItem* pTableItem = this->findItem(pid);
		if(pTableItem == NULL)
		{
			ERROR_MSG(fmt::format("EntityTable::writeTable: not found item[{}].\n", pid));
			return dbid;
		}
		
		static_cast<EntityTableItemMysqlBase*>(pTableItem)->getWriteSqlItem(dbi, s, context);
	};

	if(!WriteEntityHelper::writeDB(context.dbid > 0 ? TABLE_OP_UPDATE : TABLE_OP_INSERT, 
		dbi, context))
		return 0;

	dbid = context.dbid;

	// ���dbidΪ0��洢ʧ�ܷ���
	if(dbid <= 0)
		return dbid;

	// ����ʵ���Ƿ��Զ�����
	if(shouldAutoLoad > -1)
		entityShouldAutoLoad(dbi, dbid, shouldAutoLoad > 0);

	return dbid;
}

//-------------------------------------------------------------------------------------
bool EntityTableMysql::removeEntity(DBInterface* dbi, DBID dbid)
{
	KBE_ASSERT(pModule && dbid > 0);

	DBContext context;
	context.parentTableName = "";
	context.parentTableDBID = 0;
	context.dbid = dbid;
	context.tableName = pModule->getName();
	context.isEmpty = false;
	context.readresultIdx = 0;

	std::vector<EntityTableItem*>::iterator iter = tableFixedOrderItems_.begin();
	for(; iter != tableFixedOrderItems_.end(); ++iter)
	{
		static_cast<EntityTableItemMysqlBase*>((*iter))->getReadSqlItem(context);
	}

	bool ret = RemoveEntityHelper::removeDB(dbi, context);
	KBE_ASSERT(ret);

	return true;
}

//-------------------------------------------------------------------------------------
bool EntityTableMysql::queryTable(DBInterface* dbi, DBID dbid, MemoryStream* s)
{
	KBE_ASSERT(pModule && s && dbid > 0);

	DBContext context;
	context.parentTableName = "";
	context.parentTableDBID = 0;
	context.dbid = dbid;
	context.tableName = pModule->getName();
	context.isEmpty = false;
	context.readresultIdx = 0;

	std::vector<EntityTableItem*>::iterator iter = tableFixedOrderItems_.begin();
	for(; iter != tableFixedOrderItems_.end(); ++iter)
	{
		static_cast<EntityTableItemMysqlBase*>((*iter))->getReadSqlItem(context);
	}

	if(!ReadEntityHelper::queryDB(dbi, context))
		return false;

	if(context.dbids[dbid].size() == 0)
		return false;

	iter = tableFixedOrderItems_.begin();
	for(; iter != tableFixedOrderItems_.end(); ++iter)
	{
		static_cast<EntityTableItemMysqlBase*>((*iter))->addToStream(s, context, dbid);
	}

	return context.dbid == dbid;
}

//-------------------------------------------------------------------------------------
void EntityTableMysql::addToStream(MemoryStream* s, DBContext& context, DBID resultDBID)
{
	std::vector<EntityTableItem*>::iterator iter = tableFixedOrderItems_.begin();
	for(; iter != tableFixedOrderItems_.end(); ++iter)
	{
		static_cast<EntityTableItemMysqlBase*>((*iter))->addToStream(s, context, resultDBID);
	}
}

//-------------------------------------------------------------------------------------
void EntityTableMysql::getWriteSqlItem(DBInterface* dbi, MemoryStream* s, DBContext& context)
{
	if(tableFixedOrderItems_.size() == 0)
		return;
	
	std::vector<EntityTableItem*>::iterator iter = tableFixedOrderItems_.begin();

	DBContext* context1 = new DBContext();
	context1->parentTableName = (*iter)->pParentTable()->tableName();
	context1->tableName = (*iter)->tableName();
	context1->parentTableDBID = 0;
	context1->dbid = 0;
	context1->isEmpty = (s == NULL);
	context1->readresultIdx = 0;

	KBEShared_ptr< DBContext > opTableValBox1Ptr(context1);
	context.optable.push_back( std::pair<std::string/*tableName*/, KBEShared_ptr< DBContext > >
		((*iter)->tableName(), opTableValBox1Ptr));

	for(; iter != tableFixedOrderItems_.end(); ++iter)
	{
		static_cast<EntityTableItemMysqlBase*>((*iter))->getWriteSqlItem(dbi, s, *context1);
	}
}

//-------------------------------------------------------------------------------------
void EntityTableMysql::getReadSqlItem(DBContext& context)
{
	if(tableFixedOrderItems_.size() == 0)
		return;
	
	std::vector<EntityTableItem*>::iterator iter = tableFixedOrderItems_.begin();

	DBContext* context1 = new DBContext();
	context1->parentTableName = (*iter)->pParentTable()->tableName();
	context1->tableName = (*iter)->tableName();
	context1->parentTableDBID = 0;
	context1->dbid = 0;
	context1->isEmpty = true;
	context1->readresultIdx = 0;

	KBEShared_ptr< DBContext > opTableValBox1Ptr(context1);
	context.optable.push_back( std::pair<std::string/*tableName*/, KBEShared_ptr< DBContext > >
		((*iter)->tableName(), opTableValBox1Ptr));

	for(; iter != tableFixedOrderItems_.end(); ++iter)
	{
		static_cast<EntityTableItemMysqlBase*>((*iter))->getReadSqlItem(*context1);
	}
}

//-------------------------------------------------------------------------------------
void EntityTableItemMysqlBase::init_db_item_name(const char* exstrFlag)
{
	kbe_snprintf(db_item_name_, MAX_BUF, TABLE_ITEM_PERFIX"_%s%s", exstrFlag, itemName());
}

//-------------------------------------------------------------------------------------
bool EntityTableItemMysql_ARRAY::isSameKey(std::string key)
{
	return false;
}

//-------------------------------------------------------------------------------------
bool EntityTableItemMysql_ARRAY::initialize(std::string name)
{
	bool ret = EntityTableItemMysqlBase::initialize(name);
	if(!ret)
		return false;

	// �����ӱ�
	EntityTableMysql* pTable = new EntityTableMysql();

	std::string tname = this->pParentTable()->tableName();
	std::vector<std::string> qname;
	EntityTableItem* pparentItem = this->pParentTableItem();
	while(pparentItem != NULL)
	{
		if(strlen(pparentItem->itemName()) > 0)
			qname.push_back(pparentItem->itemName());
		pparentItem = pparentItem->pParentTableItem();
	}
	
	if(qname.size() > 0)
	{
		for(int i = (int)qname.size() - 1; i >= 0; i--)
		{
			tname += "_";
			tname += qname[i];
		}
	}
	
	std::string tableName = tname + "_";
	std::string itemName = "";

	if(name.size() > 0)
	{
		tableName += name;
	}
	else
	{
		tableName += TABLE_ARRAY_ITEM_VALUES_CONST_STR;
	}

	if(itemName.size() == 0)
	{
		if(static_cast<FixedArrayType*>(const_cast<DataType*>(pDataType))->getDataType()->type() != DATA_TYPE_FIXEDDICT)
			itemName = TABLE_ARRAY_ITEM_VALUE_CONST_STR;
	}

	pTable->tableName(tableName);
	pTable->isChild(true);

	EntityTableItem* pArrayTableItem;
	pArrayTableItem = pParentTable_->createItem(static_cast<FixedArrayType*>(const_cast<DataType*>(pDataType))->getDataType()->getName());
	pArrayTableItem->utype(-pPropertyDescription->getUType());
	pArrayTableItem->pParentTable(this->pParentTable());
	pArrayTableItem->pParentTableItem(this);
	pArrayTableItem->tableName(pTable->tableName());

	ret = pArrayTableItem->initialize(pPropertyDescription, 
		static_cast<FixedArrayType*>(const_cast<DataType*>(pDataType))->getDataType(), itemName.c_str());

	if(!ret)
	{
		delete pTable;
		return ret;
	}

	pTable->addItem(pArrayTableItem);
	pChildTable_ = pTable;

	EntityTables::getSingleton().addTable(pTable);
	return true;
}

//-------------------------------------------------------------------------------------
bool EntityTableItemMysql_ARRAY::syncToDB(DBInterface* dbi, void* pData)
{
	if(pChildTable_)
		return pChildTable_->syncToDB(dbi);

	return true;
}

//-------------------------------------------------------------------------------------
void EntityTableItemMysql_ARRAY::addToStream(MemoryStream* s, DBContext& context, DBID resultDBID)
{
	if(pChildTable_)
	{
		DBContext::DB_RW_CONTEXTS::iterator iter = context.optable.begin();
		for(; iter != context.optable.end(); ++iter)
		{
			if(pChildTable_->tableName() == iter->first)
			{
				ArraySize size = (ArraySize)iter->second->dbids[resultDBID].size();
				(*s) << size;

				for(ArraySize i=0; i<size; ++i)
					static_cast<EntityTableMysql*>(pChildTable_)->addToStream(s, *iter->second.get(), iter->second->dbids[resultDBID][i]);

				return;
			}
		}
	}

	ArraySize size = 0;
	(*s) << size;
}

//-------------------------------------------------------------------------------------
void EntityTableItemMysql_ARRAY::getWriteSqlItem(DBInterface* dbi, MemoryStream* s, DBContext& context)
{
	ArraySize size = 0;
	if(s)
		(*s) >> size;

	if(pChildTable_)
	{
		if(size > 0)
		{
			for(ArraySize i=0; i<size; ++i)
				static_cast<EntityTableMysql*>(pChildTable_)->getWriteSqlItem(dbi, s, context);
		}
		else
		{
			static_cast<EntityTableMysql*>(pChildTable_)->getWriteSqlItem(dbi, NULL, context);
		}
	}
}

//-------------------------------------------------------------------------------------
void EntityTableItemMysql_ARRAY::getReadSqlItem(DBContext& context)
{
	if(pChildTable_)
	{
		static_cast<EntityTableMysql*>(pChildTable_)->getReadSqlItem(context);
	}
}

//-------------------------------------------------------------------------------------
void EntityTableItemMysql_ARRAY::init_db_item_name(const char* exstrFlag)
{
	if(pChildTable_)
	{
		static_cast<EntityTableMysql*>(pChildTable_)->init_db_item_name();
	}
}

//-------------------------------------------------------------------------------------
bool EntityTableItemMysql_FIXED_DICT::isSameKey(std::string key)
{
	FIXEDDICT_KEYTYPES::iterator fditer = keyTypes_.begin();
	bool tmpfound = false;

	for(; fditer != keyTypes_.end(); ++fditer)
	{
		if(fditer->second->isSameKey(key))
		{
			tmpfound = true;
			break;
		}
	}
	
	return tmpfound;
}

//-------------------------------------------------------------------------------------
bool EntityTableItemMysql_FIXED_DICT::initialize(std::string name)
{
	bool ret = EntityTableItemMysqlBase::initialize(pPropertyDescription, pDataType, name);
	if(!ret)
		return false;

	KBEngine::FixedDictType* fdatatype = static_cast<KBEngine::FixedDictType*>(const_cast<DataType*>(pDataType));

	FixedDictType::FIXEDDICT_KEYTYPE_MAP& keyTypes = fdatatype->getKeyTypes();
	FixedDictType::FIXEDDICT_KEYTYPE_MAP::iterator iter = keyTypes.begin();

	for(; iter != keyTypes.end(); ++iter)
	{
		if(!iter->second->persistent)
			continue;

		EntityTableItem* tableItem = pParentTable_->createItem(iter->second->dataType->getName());

		tableItem->pParentTable(this->pParentTable());
		tableItem->pParentTableItem(this);
		tableItem->utype(-pPropertyDescription->getUType());
		tableItem->tableName(this->tableName());
		if(!tableItem->initialize(pPropertyDescription, iter->second->dataType, iter->first))
			return false;

		std::pair< std::string, KBEShared_ptr<EntityTableItem> > itemVal;
		itemVal.first = iter->first;
		itemVal.second.reset(tableItem);

		keyTypes_.push_back(itemVal);
	}

	return true;
}

//-------------------------------------------------------------------------------------
bool EntityTableItemMysql_FIXED_DICT::syncToDB(DBInterface* dbi, void* pData)
{
	EntityTableItemMysql_FIXED_DICT::FIXEDDICT_KEYTYPES::iterator iter = keyTypes_.begin();
	for(; iter != keyTypes_.end(); ++iter)
	{
		if(!iter->second->syncToDB(dbi, pData))
			return false;
	}

	return true;
}

//-------------------------------------------------------------------------------------
void EntityTableItemMysql_FIXED_DICT::addToStream(MemoryStream* s, DBContext& context, DBID resultDBID)
{
	FIXEDDICT_KEYTYPES::iterator fditer = keyTypes_.begin();

	for(; fditer != keyTypes_.end(); ++fditer)
	{
		static_cast<EntityTableItemMysqlBase*>(fditer->second.get())->addToStream(s, context, resultDBID);
	}
}

//-------------------------------------------------------------------------------------
void EntityTableItemMysql_FIXED_DICT::getWriteSqlItem(DBInterface* dbi, MemoryStream* s, DBContext& context)
{
	FIXEDDICT_KEYTYPES::iterator fditer = keyTypes_.begin();

	for(; fditer != keyTypes_.end(); ++fditer)
	{
		static_cast<EntityTableItemMysqlBase*>(fditer->second.get())->getWriteSqlItem(dbi, s, context);
	}
}

//-------------------------------------------------------------------------------------
void EntityTableItemMysql_FIXED_DICT::getReadSqlItem(DBContext& context)
{
	FIXEDDICT_KEYTYPES::iterator fditer = keyTypes_.begin();

	for(; fditer != keyTypes_.end(); ++fditer)
	{
		static_cast<EntityTableItemMysqlBase*>(fditer->second.get())->getReadSqlItem(context);
	}
}

//-------------------------------------------------------------------------------------
void EntityTableItemMysql_FIXED_DICT::init_db_item_name(const char* exstrFlag)
{
	FIXEDDICT_KEYTYPES::iterator fditer = keyTypes_.begin();

	for(; fditer != keyTypes_.end(); ++fditer)
	{
		std::string new_exstrFlag = exstrFlag;
		if(fditer->second->type()== TABLE_ITEM_TYPE_FIXEDDICT)
			new_exstrFlag += fditer->first + "_";

		static_cast<EntityTableItemMysqlBase*>(fditer->second.get())->init_db_item_name(new_exstrFlag.c_str());
	}
}

//-------------------------------------------------------------------------------------
bool EntityTableItemMysqlBase::initialize(std::string name)
{
	itemName(name);

	pDataType_ = pDataType;
	pPropertyDescription_ = pPropertyDescription;
	indexType_ = pPropertyDescription->indexType();
	return true;
}

//-------------------------------------------------------------------------------------
bool EntityTableItemMysql_DIGIT::syncToDB(DBInterface* dbi, void* pData)
{
	if(datalength_ == 0)
	{
		return sync_item_to_db(dbi, itemDBType_.c_str(), tableName_.c_str(), 
			db_item_name(), datalength_, this->mysqlItemtype_, this->flags(), pData);
	}

	uint32 length = pPropertyDescription_->getDatabaseLength();
	char sql_str[MAX_BUF];

	if(length <= 0)
		kbe_snprintf(sql_str, MAX_BUF, "%s", itemDBType_.c_str());
	else
		kbe_snprintf(sql_str, MAX_BUF, "%s(%u)", itemDBType_.c_str(), length);

	return sync_item_to_db(dbi, sql_str, tableName_.c_str(), db_item_name(), length, this->mysqlItemtype_, this->flags(), pData);
}

//-------------------------------------------------------------------------------------
void EntityTableItemMysql_DIGIT::addToStream(MemoryStream* s, DBContext& context, DBID resultDBID)
{
	std::stringstream stream;
	stream << context.results[context.readresultIdx++];

	if(dataSType_ == "INT8")
	{
		int32 v;
		stream >> v;
		int8 vv = static_cast<int8>(v);
		(*s) << vv;
	}
	else if(dataSType_ == "INT16")
	{
		int16 v;
		stream >> v;
		(*s) << v;
	}
	else if(dataSType_ == "INT32")
	{
		int32 v;
		stream >> v;
		(*s) << v;
	}
	else if(dataSType_ == "INT64")
	{
		int64 v;
		stream >> v;
		(*s) << v;
	}
	else if(dataSType_ == "UINT8")
	{
		int32 v;
		stream >> v;
		uint8 vv = static_cast<uint8>(v);
		(*s) << vv;
	}
	else if(dataSType_ == "UINT16")
	{
		uint16 v;
		stream >> v;
		(*s) << v;
	}
	else if(dataSType_ == "UINT32")
	{
		uint32 v;
		stream >> v;
		(*s) << v;
	}
	else if(dataSType_ == "UINT64")
	{
		uint64 v;
		stream >> v;
		(*s) << v;
	}
	else if(dataSType_ == "FLOAT")
	{
		float v;
		stream >> v;
		(*s) << v;
	}
	else if(dataSType_ == "DOUBLE")
	{
		double v;
		stream >> v;
		(*s) << v;
	}
}

//-------------------------------------------------------------------------------------
void EntityTableItemMysql_DIGIT::getWriteSqlItem(DBInterface* dbi, MemoryStream* s, DBContext& context)
{
	if(s == NULL)
		return;

	DBContext::DB_ITEM_DATA* pSotvs = new DBContext::DB_ITEM_DATA();
	
	if(dataSType_ == "INT8")
	{
		int8 v;
		(*s) >> v;
		kbe_snprintf(pSotvs->sqlval, MAX_BUF, "%d", v);
	}
	else if(dataSType_ == "INT16")
	{
		int16 v;
		(*s) >> v;
		kbe_snprintf(pSotvs->sqlval, MAX_BUF, "%d", v);
	}
	else if(dataSType_ == "INT32")
	{
		int32 v;
		(*s) >> v;
		kbe_snprintf(pSotvs->sqlval, MAX_BUF, "%d", v);
	}
	else if(dataSType_ == "INT64")
	{
		int64 v;
		(*s) >> v;
		kbe_snprintf(pSotvs->sqlval, MAX_BUF, "%" PRI64, v);
	}
	else if(dataSType_ == "UINT8")
	{
		uint8 v;
		(*s) >> v;
		kbe_snprintf(pSotvs->sqlval, MAX_BUF, "%u", v);
	}
	else if(dataSType_ == "UINT16")
	{
		uint16 v;
		(*s) >> v;
		kbe_snprintf(pSotvs->sqlval, MAX_BUF, "%u", v);
	}
	else if(dataSType_ == "UINT32")
	{
		uint32 v;
		(*s) >> v;
		kbe_snprintf(pSotvs->sqlval, MAX_BUF, "%u", v);
	}
	else if(dataSType_ == "UINT64")
	{
		uint64 v;
		(*s) >> v;
		kbe_snprintf(pSotvs->sqlval, MAX_BUF, "%" PRIu64, v);
	}
	else if(dataSType_ == "FLOAT")
	{
		float v;
		(*s) >> v;
		kbe_snprintf(pSotvs->sqlval, MAX_BUF, "%f", v);
	}
	else if(dataSType_ == "DOUBLE")
	{
		double v;
		(*s) >> v;
		kbe_snprintf(pSotvs->sqlval, MAX_BUF, "%lf", v);
	}

	pSotvs->sqlkey = db_item_name();
	context.items.push_back(KBEShared_ptr<DBContext::DB_ITEM_DATA>(pSotvs));
}

//-------------------------------------------------------------------------------------
void EntityTableItemMysql_DIGIT::getReadSqlItem(DBContext& context)
{
	DBContext::DB_ITEM_DATA* pSotvs = new DBContext::DB_ITEM_DATA();
	pSotvs->sqlkey = db_item_name();
	memset(pSotvs->sqlval, 0, MAX_BUF);
	context.items.push_back(KBEShared_ptr<DBContext::DB_ITEM_DATA>(pSotvs));
}

//-------------------------------------------------------------------------------------
bool EntityTableItemMysql_STRING::syncToDB(DBInterface* dbi, void* pData)
{
	uint32 length = pPropertyDescription_->getDatabaseLength();
	char sql_str[MAX_BUF];

	if(length > 0)
	{
		kbe_snprintf(sql_str, MAX_BUF, "%s(%u)", itemDBType_.c_str(), length);
	}
	else
	{
		kbe_snprintf(sql_str, MAX_BUF, "%s", itemDBType_.c_str());
	}

	return sync_item_to_db(dbi, sql_str, tableName_.c_str(), db_item_name(), length, 
		this->mysqlItemtype_, this->flags(), pData);
}

//-------------------------------------------------------------------------------------
void EntityTableItemMysql_STRING::addToStream(MemoryStream* s, 
											  DBContext& context, DBID resultDBID)
{
	(*s) << context.results[context.readresultIdx++];
}

//-------------------------------------------------------------------------------------
void EntityTableItemMysql_STRING::getWriteSqlItem(DBInterface* dbi, 
												  MemoryStream* s, DBContext& context)
{
	if(s == NULL)
		return;

	DBContext::DB_ITEM_DATA* pSotvs = new DBContext::DB_ITEM_DATA();

	std::string val;
	(*s) >> val;

	pSotvs->extraDatas = "\"";

	char* tbuf = new char[val.size() * 2 + 1];
	memset(tbuf, 0, val.size() * 2 + 1);

	mysql_real_escape_string(static_cast<DBInterfaceMysql*>(dbi)->mysql(), 
		tbuf, val.c_str(), val.size());

	pSotvs->extraDatas += tbuf;
	pSotvs->extraDatas += "\"";

	SAFE_RELEASE_ARRAY(tbuf);

	memset(pSotvs, 0, sizeof(pSotvs->sqlval));
	pSotvs->sqlkey = db_item_name();
	context.items.push_back(KBEShared_ptr<DBContext::DB_ITEM_DATA>(pSotvs));
}

//-------------------------------------------------------------------------------------
void EntityTableItemMysql_STRING::getReadSqlItem(DBContext& context)
{
	DBContext::DB_ITEM_DATA* pSotvs = new DBContext::DB_ITEM_DATA();
	pSotvs->sqlkey = db_item_name();
	memset(pSotvs->sqlval, 0, MAX_BUF);
	context.items.push_back(KBEShared_ptr<DBContext::DB_ITEM_DATA>(pSotvs));
}

//-------------------------------------------------------------------------------------
bool EntityTableItemMysql_UNICODE::syncToDB(DBInterface* dbi, void* pData)
{
	uint32 length = pPropertyDescription_->getDatabaseLength();
	char sql_str[MAX_BUF];

	if(length > 0)
	{
		kbe_snprintf(sql_str, MAX_BUF, "%s(%u)", itemDBType_.c_str(), length);
	}
	else
	{
		kbe_snprintf(sql_str, MAX_BUF, "%s", itemDBType_.c_str());
	}

	return sync_item_to_db(dbi, sql_str, tableName_.c_str(), db_item_name(), length, 
		this->mysqlItemtype_, this->flags(), pData);
}

//-------------------------------------------------------------------------------------
void EntityTableItemMysql_UNICODE::addToStream(MemoryStream* s, 
											   DBContext& context, DBID resultDBID)
{
	s->appendBlob(context.results[context.readresultIdx++]);
}

//-------------------------------------------------------------------------------------
void EntityTableItemMysql_UNICODE::getWriteSqlItem(DBInterface* dbi, MemoryStream* s, 
												   DBContext& context)
{
	if(s == NULL)
		return;

	DBContext::DB_ITEM_DATA* pSotvs = new DBContext::DB_ITEM_DATA();

	std::string val;
	s->readBlob(val);

	char* tbuf = new char[val.size() * 2 + 1];
	memset(tbuf, 0, val.size() * 2 + 1);

	mysql_real_escape_string(static_cast<DBInterfaceMysql*>(dbi)->mysql(), 
		tbuf, val.c_str(), val.size());
	
	pSotvs->extraDatas = "\"";
	pSotvs->extraDatas += tbuf;
	pSotvs->extraDatas += "\"";
	SAFE_RELEASE_ARRAY(tbuf);

	memset(pSotvs, 0, sizeof(pSotvs->sqlval));
	pSotvs->sqlkey = db_item_name();
	context.items.push_back(KBEShared_ptr<DBContext::DB_ITEM_DATA>(pSotvs));
}

//-------------------------------------------------------------------------------------
void EntityTableItemMysql_UNICODE::getReadSqlItem(DBContext& context)
{
	DBContext::DB_ITEM_DATA* pSotvs = new DBContext::DB_ITEM_DATA();
	pSotvs->sqlkey = db_item_name();
	memset(pSotvs->sqlval, 0, MAX_BUF);
	context.items.push_back(KBEShared_ptr<DBContext::DB_ITEM_DATA>(pSotvs));
}

//-------------------------------------------------------------------------------------
bool EntityTableItemMysql_BLOB::syncToDB(DBInterface* dbi, void* pData)
{
	return sync_item_to_db(dbi, itemDBType_.c_str(), tableName_.c_str(), db_item_name(), 0, 
		this->mysqlItemtype_, this->flags(), pData);
}

//-------------------------------------------------------------------------------------
void EntityTableItemMysql_BLOB::addToStream(MemoryStream* s, DBContext& context, DBID resultDBID)
{
	std::string& datas = context.results[context.readresultIdx++];
	s->appendBlob(datas.data(), datas.size());
}

//-------------------------------------------------------------------------------------
void EntityTableItemMysql_BLOB::getWriteSqlItem(DBInterface* dbi, MemoryStream* s, DBContext& context)
{
	if(s == NULL)
		return;

	DBContext::DB_ITEM_DATA* pSotvs = new DBContext::DB_ITEM_DATA();

	std::string val;
	s->readBlob(val);

	char* tbuf = new char[val.size() * 2 + 1];
	memset(tbuf, 0, val.size() * 2 + 1);

	mysql_real_escape_string(static_cast<DBInterfaceMysql*>(dbi)->mysql(), 
		tbuf, val.data(), val.size());

	pSotvs->extraDatas = "\"";
	pSotvs->extraDatas += tbuf;
	pSotvs->extraDatas += "\"";
	SAFE_RELEASE_ARRAY(tbuf);

	memset(pSotvs, 0, sizeof(pSotvs->sqlval));
	pSotvs->sqlkey = db_item_name();
	context.items.push_back(KBEShared_ptr<DBContext::DB_ITEM_DATA>(pSotvs));
}

//-------------------------------------------------------------------------------------
void EntityTableItemMysql_BLOB::getReadSqlItem(DBContext& context)
{
	DBContext::DB_ITEM_DATA* pSotvs = new DBContext::DB_ITEM_DATA();
	pSotvs->sqlkey = db_item_name();
	memset(pSotvs->sqlval, 0, MAX_BUF);
	context.items.push_back(KBEShared_ptr<DBContext::DB_ITEM_DATA>(pSotvs));
}

//-------------------------------------------------------------------------------------
}