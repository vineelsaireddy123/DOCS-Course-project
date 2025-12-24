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
