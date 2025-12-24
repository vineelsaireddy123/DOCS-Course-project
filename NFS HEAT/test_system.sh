#!/bin/bash

# Color codes for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}Distributed File System - Test Suite${NC}"
echo -e "${GREEN}========================================${NC}"

# Kill any existing servers
echo -e "\n${YELLOW}Cleaning up existing servers...${NC}"
pkill -f naming_server 2>/dev/null
pkill -f storage_server 2>/dev/null
sleep 1
echo -e "${GREEN}✓ Cleanup complete${NC}"

# Create storage directories
echo -e "\n${YELLOW}Creating storage directories...${NC}"
mkdir -p storage1 storage2 storage3

# Create some test files
echo "echo 'Hello from script'" > storage1/test_script.txt
echo "This is a test file." > storage1/test1.txt
echo "Another test file for reading." > storage1/test2.txt

echo -e "${GREEN}✓ Storage directories created${NC}"

# Compile the project
echo -e "\n${YELLOW}Compiling project...${NC}"
make clean
make

if [ $? -eq 0 ]; then
    echo -e "${GREEN}✓ Compilation successful${NC}"
else
    echo -e "${RED}✗ Compilation failed${NC}"
    exit 1
fi

# Start Naming Server
echo -e "\n${YELLOW}Starting Naming Server on port 8080...${NC}"
./naming_server 8080 > nm.log 2>&1 &
NM_PID=$!
sleep 2

if ps -p $NM_PID > /dev/null; then
    echo -e "${GREEN}✓ Naming Server started (PID: $NM_PID)${NC}"
else
    echo -e "${RED}✗ Naming Server failed to start${NC}"
    exit 1
fi

# Start Storage Servers
echo -e "\n${YELLOW}Starting Storage Server 1 on port 9001...${NC}"
./storage_server 127.0.0.1 8080 9001 storage1 > ss1.log 2>&1 &
SS1_PID=$!
sleep 1

if ps -p $SS1_PID > /dev/null; then
    echo -e "${GREEN}✓ Storage Server 1 started (PID: $SS1_PID)${NC}"
else
    echo -e "${RED}✗ Storage Server 1 failed to start${NC}"
fi

echo -e "\n${YELLOW}Starting Storage Server 2 on port 9002...${NC}"
./storage_server 127.0.0.1 8080 9002 storage2 > ss2.log 2>&1 &
SS2_PID=$!
sleep 1

if ps -p $SS2_PID > /dev/null; then
    echo -e "${GREEN}✓ Storage Server 2 started (PID: $SS2_PID)${NC}"
else
    echo -e "${RED}✗ Storage Server 2 failed to start${NC}"
fi

# Test scenarios
echo -e "\n${GREEN}========================================${NC}"
echo -e "${GREEN}System is ready for testing!${NC}"
echo -e "${GREEN}========================================${NC}"

echo -e "\n${YELLOW}To test the system:${NC}"
echo "1. Open a new terminal and run: ./client 127.0.0.1 8080"
echo "2. Enter a username (e.g., user1)"
echo "3. Try these commands:"
echo "   - VIEW"
echo "   - READ test1.txt"
echo "   - CREATE newfile.txt"
echo "   - WRITE newfile.txt 0"
echo "   - LIST"
echo "   - INFO test1.txt"
echo ""
echo "4. Open another terminal for a second client to test concurrency"

echo -e "\n${YELLOW}Logs are being written to:${NC}"
echo "  - nm.log (Naming Server)"
echo "  - ss1.log (Storage Server 1)"
echo "  - ss2.log (Storage Server 2)"
echo "  - system.log (Combined system log)"

echo -e "\n${YELLOW}To stop all servers:${NC}"
echo "  bash stop_servers.sh"

# Create stop script
cat > stop_servers.sh << 'EOF'
#!/bin/bash

echo "Stopping all servers..."

# Kill naming server
NM_PID=$(pgrep -f "naming_server 8080")
if [ ! -z "$NM_PID" ]; then
    kill $NM_PID
    echo "Stopped Naming Server (PID: $NM_PID)"
fi

# Kill storage servers
SS_PIDS=$(pgrep -f "storage_server")
for PID in $SS_PIDS; do
    kill $PID
    echo "Stopped Storage Server (PID: $PID)"
done

echo "All servers stopped."
EOF

chmod +x stop_servers.sh

echo -e "\n${GREEN}========================================${NC}"
echo -e "${GREEN}Press Ctrl+C when done testing${NC}"
echo -e "${GREEN}========================================${NC}"

# Wait for user interrupt
wait $NM_PID