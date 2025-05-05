#!/bin/bash

echo "Setting up ESP32 Access Control System..."

# Navigate to backend directory
cd backend

# Install dependencies
echo "Installing backend dependencies..."
npm install

# Go back to root
cd ..

echo "Setup complete!"
echo "To start the server, run: ./start.sh"
echo ""
echo "Default login credentials:"
echo "  Username: admin"
echo "  Password: esp32admin"
