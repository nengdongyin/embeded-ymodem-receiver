#!/bin/bash
# build_and_test.sh
# Build and run all ymodem unit tests.
# Run from project root (ymodem_receiver/).

set -e

UNITY_CORE="test/unity/unity_core"
MOCK_DIR="test/unity/mocks"
UNIT_DIR="test/unity/unit"
SRC_DIR="src"

CFLAGS="-I${SRC_DIR} -I${UNITY_CORE} -Wall -Wextra"
UNITY_SRC="${UNITY_CORE}/unity.c"
MOCK_SRC="${MOCK_DIR}/mock_time.c"

echo "=== Ymodem Unit Tests ==="
echo ""

# ---------------------------------------
# CRC tests
# ---------------------------------------
echo "[1/3] Building test_common..."
gcc ${CFLAGS} ${UNITY_SRC} ${MOCK_SRC} \
    ${UNIT_DIR}/test_common.c \
    ${SRC_DIR}/ymodem_common.c \
    -o ${UNIT_DIR}/test_common.exe

echo "Running test_common..."
${UNIT_DIR}/test_common.exe
echo ""

# ---------------------------------------
# Sender tests
# ---------------------------------------
echo "[2/3] Building test_sender..."
gcc ${CFLAGS} ${UNITY_SRC} ${MOCK_SRC} \
    ${UNIT_DIR}/test_sender.c \
    ${SRC_DIR}/ymodem_sender.c ${SRC_DIR}/ymodem_common.c \
    -o ${UNIT_DIR}/test_sender.exe

echo "Running test_sender..."
${UNIT_DIR}/test_sender.exe
echo ""

# ---------------------------------------
# Receiver tests
# ---------------------------------------
echo "[3/3] Building test_receiver..."
gcc ${CFLAGS} ${UNITY_SRC} ${MOCK_SRC} \
    ${UNIT_DIR}/test_receiver.c \
    ${SRC_DIR}/ymodem_receiver.c ${SRC_DIR}/ymodem_common.c \
    -o ${UNIT_DIR}/test_receiver.exe

echo "Running test_receiver..."
${UNIT_DIR}/test_receiver.exe
echo ""

echo "=== All tests passed ==="
