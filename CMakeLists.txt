CMAKE_MINIMUM_REQUIRED(VERSION 2.6)
PROJECT(ps-plugin C)

#INCLUDE(FindPkgConfig)

SET(PREFIX ${CMAKE_INSTALL_PREFIX})
SET(EXEC_PREFIX "\${prefix}")
SET(LIBDIR "\${prefix}/lib")
SET(INCLUDEDIR "\${prefix}/include")
SET(DATAROOTDIR "\${prefix}/share")
if(NOT DEFINED SYSCONFDIR)
	SET(SYSCONFDIR "/etc")
endif()

# Set required packages
INCLUDE(FindPkgConfig)
pkg_check_modules(pkgs REQUIRED glib-2.0 dbus-glib-1 dlog tcore iniparser)

FOREACH(flag ${pkgs_CFLAGS})
	SET(EXTRA_CFLAGS "${EXTRA_CFLAGS} ${flag}")
ENDFOREACH(flag)

INCLUDE_DIRECTORIES(${CMAKE_SOURCE_DIR}/include/)

SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${EXTRA_CFLAGS} -Werror -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wdeclaration-after-statement -Wmissing-declarations -Wredundant-decls -Wcast-align")

ADD_DEFINITIONS("-DFEATURE_DLOG_DEBUG")
ADD_DEFINITIONS("-DTCORE_LOG_TAG=\"PACKETSERVICE\"")

MESSAGE(${CMAKE_C_FLAGS})
MESSAGE(${CMAKE_EXE_LINKER_FLAGS})

SET(SRCS
		src/main.c
		src/master.c
		src/modem.c
		src/service.c
		src/context.c
		src/error.c
		src/tcore-interface.c
)

# library build
ADD_LIBRARY(ps-plugin SHARED ${SRCS})
TARGET_LINK_LIBRARIES(ps-plugin ${pkgs_LDFLAGS})
SET_TARGET_PROPERTIES(ps-plugin PROPERTIES PREFIX "" OUTPUT_NAME ps-plugin)

ADD_CUSTOM_TARGET(ps-iface-master-glue.h
	COMMAND dbus-binding-tool --mode=glib-server --prefix=ps_iface_master
	--output=${CMAKE_SOURCE_DIR}/include/ps-iface-master-glue.h
	${CMAKE_SOURCE_DIR}/interfaces/ps-iface-master.xml
	DEPENDS ${CMAKE_SOURCE_DIR}/interfaces/ps-iface-master.xml
)
ADD_CUSTOM_TARGET(ps-iface-modem-glue.h
	COMMAND dbus-binding-tool --mode=glib-server --prefix=ps_iface_modem
	--output=${CMAKE_SOURCE_DIR}/include/ps-iface-modem-glue.h
	${CMAKE_SOURCE_DIR}/interfaces/ps-iface-modem.xml
	DEPENDS ${CMAKE_SOURCE_DIR}/interfaces/ps-iface-modem.xml
)
ADD_CUSTOM_TARGET(ps-iface-service-glue.h
	COMMAND dbus-binding-tool --mode=glib-server --prefix=ps_iface_service
	--output=${CMAKE_SOURCE_DIR}/include/ps-iface-service-glue.h
	${CMAKE_SOURCE_DIR}/interfaces/ps-iface-service.xml
	DEPENDS ${CMAKE_SOURCE_DIR}/interfaces/ps-iface-service.xml
)
ADD_CUSTOM_TARGET(ps-iface-context-glue.h
	COMMAND dbus-binding-tool --mode=glib-server --prefix=ps_iface_context
	--output=${CMAKE_SOURCE_DIR}/include/ps-iface-context-glue.h
	${CMAKE_SOURCE_DIR}/interfaces/ps-iface-context.xml
	DEPENDS ${CMAKE_SOURCE_DIR}/interfaces/ps-iface-context.xml
)

ADD_DEPENDENCIES(${PROJECT_NAME} ps-iface-master-glue.h)
ADD_DEPENDENCIES(${PROJECT_NAME} ps-iface-modem-glue.h)
ADD_DEPENDENCIES(${PROJECT_NAME} ps-iface-service-glue.h)
ADD_DEPENDENCIES(${PROJECT_NAME} ps-iface-context-glue.h)

# install
INSTALL(FILES ${CMAKE_SOURCE_DIR}/src/ps-plugin.conf DESTINATION ${SYSCONFDIR}/dbus-1/system.d)
INSTALL(FILES ${CMAKE_SOURCE_DIR}/resources/dnet_db.sql DESTINATION ${DATAROOTDIR}/ps-plugin)
INSTALL(FILES ${CMAKE_SOURCE_DIR}/resources/dnet_db_data.sql DESTINATION ${DATAROOTDIR}/ps-plugin)
INSTALL(TARGETS ps-plugin LIBRARY DESTINATION ${LIB_INSTALL_DIR}/telephony/plugins)
INSTALL(FILES ${CMAKE_SOURCE_DIR}/LICENSE DESTINATION /usr/share/license RENAME tel-plugin-packetservice)
