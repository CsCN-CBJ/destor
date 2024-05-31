#!/bin/bash

. ./utils.sh

testInit /data/cbj/destor
CONFIG=-p"working-directory $WORKING_DIR"
SRC_DIR=/data/cbj/temp

cd ~/destor
remake

resetAll
rm -r ~/destor/log/time*
mkdir -p ~/destor/log/time

[[ "$*" =~ "-chk" ]] && chk=1 || chk=0
[[ "$*" =~ "-bkp" ]] && bkp=1 || bkp=0

mysql -uroot -proot -e "
  use CBJ;
  drop table if exists kvstore;
  create table kvstore ( k BINARY(32) PRIMARY KEY, v BINARY(8));
"

set -x
# basic test
mkdir -p ${DST_DIR}${RESTORE_ID}
destor ${SRC_DIR} "${CONFIG}" > ${LOG_DIR}/${RESTORE_ID}.log
cp -r ${WORKING_DIR} ${WORKING_DIR}_bak

destor -r0 ${DST_DIR}${RESTORE_ID} "${CONFIG}"
mv ~/destor/log/time ~/destor/log/time${RESTORE_ID}
mkdir ~/destor/log/time
let ++RESTORE_ID

function update() {
    # update test
    rm -r ${WORKING_DIR}
    cp -r ${WORKING_DIR}_bak ${WORKING_DIR}
    
    mkdir -p ${DST_DIR}${RESTORE_ID}
    destor -u0 ${SRC_DIR} -i"$1" "${CONFIG}" > ${LOG_DIR}/${RESTORE_ID}.log
    rm ${WORKING_DIR}/container.pool
    destor -n1 ${DST_DIR}${RESTORE_ID} "${CONFIG}"
    mv ~/destor/log/time ~/destor/log/time${RESTORE_ID}
    mkdir ~/destor/log/time

    let ++RESTORE_ID
}

update 0

mysql -uroot -proot -e "
    use CBJ;
    drop table if exists test;
    create table test ( k BINARY(32) PRIMARY KEY, v BINARY(40));
"
update 1

mysql -uroot -proot -e "
    use CBJ;
    drop table if exists test;
    create table test ( k BINARY(8) PRIMARY KEY, v MediumBlob);
"
update 2

compareRestore

if [ ${flag} -eq 0 ]; then
    make clean -s
    echo "all test passed"
else
    echo "test failed"
    exit 1
fi
