#ifndef ROOT_SCOPE_H
#define ROOT_SCOPE_H 

#include <iostream>
#include <libconfig.h++> 

#include "xdpd/common/exception.h"

#include "scope.h"

/**
* @file root_scope.h
* @author Marc Sune<marc.sune (at) bisdn.de>
*
* @brief Root node of the configuration 
* 
*/

namespace xdpd {

class root_scope : public scope {
	
public:
	root_scope();
	virtual ~root_scope();
		
private:

};

class config_scope : public scope {
	
public:
	config_scope(scope* parent);
	virtual ~config_scope();
		
private:

};


}// namespace xdpd 

#endif /* ROOT_SCOPE_H_ */


