#!/bin/bash

# Start the server
cd backend
echo "Starting the server..."
node server.js &
SERVER_PID=$!

# Give the server some time to start
sleep 2

# Wait for user to press Ctrl+C
trap "kill $SERVER_PID; echo 'Server stopped'; exit 0" INT
wait $SERVER_PID