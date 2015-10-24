#!/bin/sh

#--------------------------------------------
#                PacketService
#-------------------------------------------

PS_DEBUG_DIR=$1/packetservice
mkdir -p ${PS_DEBUG_DIR}

cp /opt/dbspace/.dnet*.* ${PS_DEBUG_DIR}/
