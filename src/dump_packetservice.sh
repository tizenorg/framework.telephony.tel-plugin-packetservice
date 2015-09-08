#!/bin/sh

#--------------------------------------------
#                PacketService
#-------------------------------------------

PS_DEBUG_DIR=$1/packetservice
mkdir -p ${PS_DEBUG_DIR}

cp /opt/dbspace/.dnet.db ${PS_DEBUG_DIR}/dnet.db
