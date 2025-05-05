// Backend for ESP32 Access Control System
const express = require('express');
const cors = require('cors');
const fs = require('fs');
const path = require('path');
const session = require('express-session');
const cookieParser = require('cookie-parser');
const fileUpload = require('express-fileupload');
const app = express();
const PORT = 3000;

// Data persistence paths
const DATA_DIR = path.join(__dirname, 'data');
const NFC_DATA_PATH = path.join(DATA_DIR, 'nfc_cards.json');
const FINGERPRINT_DATA_PATH = path.join(DATA_DIR, 'fingerprints.json');
const BT_DATA_PATH = path.join(DATA_DIR, 'bt_devices.json');
const SECURITY_LOGS_PATH = path.join(DATA_DIR, 'security_logs.json');
const PHOTOS_DIR = path.join(DATA_DIR, 'photos');

// Ensure required directories exist
ensureDirectoriesExist();

// Configure middleware
setupMiddleware();

// Authentication configuration - Hardcoded user for simplicity
const ADMIN_USER = {
  username: 'admin',
  password: 'esp32admin'
};

// Storage for in-memory data
let nfcCards = [];
let fingerprints = [];
let btMacs = [];
let securityLogs = [];

// System status information
let systemStatus = {
  lastSync: new Date().toISOString(),
  activeESP32s: [],
  totalCards: 0,
  totalUsers: 0,
  totalBtDevices: 0,
  totalSecurityLogs: 0
};

// Log types enum
const LogType = {
  ACCESS_DENIED: 'access_denied',   // Red - Unauthorized fingerprint attempts
  MOTION_DETECTED: 'motion_detected', // Yellow - Motion sensor triggered
  ACCESS_GRANTED: 'access_granted'  // Green - Successful door access
};

// Initialize data
loadData();

// Setup API routes
setupAuthRoutes();
setupCardRoutes();
setupFingerprintRoutes();
setupBluetoothRoutes();
setupSecurityLogRoutes();
setupSystemRoutes();

// Start the server
app.listen(PORT, '0.0.0.0', () => {
  console.log(`Server running on http://0.0.0.0:${PORT}`);
  console.log(`Access the server locally at http://localhost:${PORT}`);
  console.log(`From other devices use http://YOUR_IP_ADDRESS:${PORT}`);
});

// ========== MIDDLEWARE SETUP ==========

function setupMiddleware() {
  app.use(cors());
  app.use(express.json());
  app.use(cookieParser());
  app.use(fileUpload({
    limits: { fileSize: 10 * 1024 * 1024 }, // 10 MB max file size
    createParentPath: true
  }));
  app.use(session({
    secret: 'esp32-access-control-secret-key',
    resave: false,
    saveUninitialized: false,
    cookie: { secure: false, maxAge: 3600000 } // 1 hour
  }));
  app.use(express.static(path.join(__dirname, '../frontend')));
}

// Authentication middleware
function requireAuth(req, res, next) {
  // ESP32 endpoints don't require authentication
  if (req.path === '/api/cards/uids' || 
      req.path === '/api/fingerprints' || 
      req.path === '/api/bluetooth/macs' || 
      req.path === '/api/checkin') {
    return next();
  }
  
  if (req.session && req.session.authenticated) {
    return next();
  }
  // API requests return 401, page requests redirect to login
  if (req.path.startsWith('/api/')) {
    return res.status(401).json({ error: 'Authentication required' });
  }
  return res.redirect('/login.html');
}

// ========== DATA MANAGEMENT ==========

function ensureDirectoriesExist() {
  if (!fs.existsSync(DATA_DIR)) {
    fs.mkdirSync(DATA_DIR, { recursive: true });
  }
  if (!fs.existsSync(PHOTOS_DIR)) {
    fs.mkdirSync(PHOTOS_DIR, { recursive: true });
  }
}

// Update system status
function updateSystemStatus() {
  systemStatus.lastSync = new Date().toISOString();
  systemStatus.totalCards = nfcCards.length;
  systemStatus.totalUsers = fingerprints.length;
  systemStatus.totalBtDevices = btMacs.length;
  systemStatus.totalSecurityLogs = securityLogs.length;
}

// Load data from files if they exist
function loadData() {
  try {
    nfcCards = loadJsonFile(NFC_DATA_PATH, [])
      .filter(card => {
        const isValidHex = /^[0-9A-Fa-f]+$/.test(card.uid);
        if (!isValidHex) {
          console.warn(`Skipping card with invalid UID format: ${card.uid}`);
        }
        return isValidHex;
      });
    console.log(`Loaded ${nfcCards.length} NFC cards from storage`);
    
    fingerprints = loadJsonFile(FINGERPRINT_DATA_PATH, []);
    console.log(`Loaded ${fingerprints.length} fingerprints from storage`);
    
    btMacs = loadJsonFile(BT_DATA_PATH, []);
    console.log(`Loaded ${btMacs.length} Bluetooth devices from storage`);
    
    securityLogs = loadJsonFile(SECURITY_LOGS_PATH, []);
    console.log(`Loaded ${securityLogs.length} security logs from storage`);
  } catch (error) {
    console.error('Error loading data from files:', error);
  }
}

// Helper function to load JSON data from a file
function loadJsonFile(filePath, defaultValue = []) {
  if (fs.existsSync(filePath)) {
    const data = fs.readFileSync(filePath, 'utf8');
    return JSON.parse(data);
  }
  return defaultValue;
}

// Save data to files
function saveData() {
  try {
    saveJsonFile(NFC_DATA_PATH, nfcCards);
    saveJsonFile(FINGERPRINT_DATA_PATH, fingerprints);
    saveJsonFile(BT_DATA_PATH, btMacs);
    saveJsonFile(SECURITY_LOGS_PATH, securityLogs);
    console.log('Data saved to files');
  } catch (error) {
    console.error('Error saving data to files:', error);
  }
}

// Helper function to save JSON data to a file
function saveJsonFile(filePath, data) {
  fs.writeFileSync(filePath, JSON.stringify(data, null, 2));
}

// ========== ROUTE SETUP FUNCTIONS ==========

function setupAuthRoutes() {
  // Login endpoint
  app.post('/api/login', (req, res) => {
    const { username, password } = req.body;
    
    if (username === ADMIN_USER.username && password === ADMIN_USER.password) {
      req.session.authenticated = true;
      res.status(200).json({ success: true });
    } else {
      res.status(401).json({ error: 'Invalid username or password' });
    }
  });

  // Logout endpoint
  app.get('/api/logout', (req, res) => {
    req.session.destroy();
    res.redirect('/login.html');
  });

  // Specifically serve login page without authentication
  app.get('/login.html', (req, res) => {
    res.sendFile(path.join(__dirname, '../frontend/login.html'));
  });

  // Root path redirects to login if not authenticated, otherwise serves index.html
  app.get('/', (req, res) => {
    if (req.session && req.session.authenticated) {
      res.sendFile(path.join(__dirname, '../frontend/index.html'));
    } else {
      res.redirect('/login.html');
    }
  });
}

function setupCardRoutes() {
  // GET all authorized cards
  app.get('/api/cards', requireAuth, (req, res) => {
    res.json(nfcCards);
  });

  // POST a new card
  app.post('/api/cards', requireAuth, (req, res) => {
    const { uid, name } = req.body;
    
    if (!uid) {
      return res.status(400).json({ error: 'Card UID is required' });
    }
    
    // Validate that the UID is a valid hex string
    const hexRegex = /^[0-9A-Fa-f]+$/;
    if (!hexRegex.test(uid)) {
      return res.status(400).json({ error: 'Card UID must be a valid hexadecimal string' });
    }
    
    // Check if card already exists
    if (nfcCards.some(card => card.uid === uid)) {
      return res.status(409).json({ error: 'Card already exists' });
    }
    
    const newCard = { 
      uid, 
      name: name || `Card ${uid.substring(0, 6)}`
    };
    
    nfcCards.push(newCard);
    saveData();
    res.status(201).json(newCard);
  });

  // UPDATE a card
  app.put('/api/cards/:uid', requireAuth, (req, res) => {
    const cardUid = req.params.uid;
    const { name } = req.body;
    
    const cardIndex = nfcCards.findIndex(card => card.uid === cardUid);
    
    if (cardIndex === -1) {
      return res.status(404).json({ error: 'Card not found' });
    }
    
    // Update card properties if provided
    if (name) nfcCards[cardIndex].name = name;
    
    saveData();
    res.json(nfcCards[cardIndex]);
  });

  // DELETE a card
  app.delete('/api/cards/:uid', requireAuth, (req, res) => {
    const cardUid = req.params.uid;
    const initialLength = nfcCards.length;
    
    nfcCards = nfcCards.filter(card => card.uid !== cardUid);
    saveData();
    
    if (nfcCards.length === initialLength) {
      return res.status(404).json({ error: 'Card not found' });
    }
    
    res.json({ message: 'Card deleted successfully' });
  });

  // GET endpoint for ESP32 to retrieve card UIDs
  app.get('/api/cards/uids', (req, res) => {
    // Format the response to be easy for ESP32 to parse
    const cardsList = nfcCards.map(card => ({
      uid: card.uid
    }));
    res.json({ cards: cardsList });
  });
  
  // POST endpoint específico para ESP32-CAM para registrar tarjetas NFC sin autorización
  app.post('/api/esp32/cards', (req, res) => {
    const { uid, name } = req.body;
    
    if (!uid) {
      return res.status(400).json({ error: 'Card UID is required' });
    }
    
    // Validate that the UID is a valid hex string
    const hexRegex = /^[0-9A-Fa-f]+$/;
    if (!hexRegex.test(uid)) {
      return res.status(400).json({ error: 'Card UID must be a valid hexadecimal string' });
    }
    
    // Check if card already exists
    if (nfcCards.some(card => card.uid === uid)) {
      return res.status(409).json({ error: 'Card already exists', message: 'Esta tarjeta ya existe en el sistema' });
    }
    
    const newCard = { 
      uid, 
      name: name || `Card ${uid.substring(0, 6)}`
    };
    
    nfcCards.push(newCard);
    saveData();
    
    console.log(`[ESP32] New NFC card registered: UID ${uid}, Name: ${newCard.name}`);
    
    res.status(201).json(newCard);
  });
}

function setupFingerprintRoutes() {
  // GET all fingerprints
  app.get('/api/fingerprints', requireAuth, (req, res) => {
    res.json(fingerprints);
  });

  // POST a new fingerprint user
  app.post('/api/fingerprints', requireAuth, (req, res) => {
    const { name, fingerprintId } = req.body;
    
    if (!name || !fingerprintId) {
      return res.status(400).json({ error: 'Name and fingerprint ID are required' });
    }
    
    // Check if fingerprint already exists
    if (fingerprints.some(fp => fp.fingerprintId === fingerprintId)) {
      return res.status(409).json({ error: 'Fingerprint ID already exists' });
    }
    
    // Find the next available ID
    const nextId = fingerprints.length > 0 
      ? Math.max(...fingerprints.map(fp => fp.id)) + 1 
      : 1;
    
    const newFingerprint = {
      id: nextId,
      name: name,
      fingerprintId: fingerprintId,
      registered: new Date().toISOString()
    };
    
    fingerprints.push(newFingerprint);
    saveData();
    res.status(201).json(newFingerprint);
  });

  // UPDATE a fingerprint user
  app.put('/api/fingerprints/:id', requireAuth, (req, res) => {
    const id = parseInt(req.params.id);
    const { name } = req.body;
    
    const userIndex = fingerprints.findIndex(fp => fp.id === id);
    
    if (userIndex === -1) {
      return res.status(404).json({ error: 'Fingerprint user not found' });
    }
    
    // Update properties if provided
    if (name) fingerprints[userIndex].name = name;
    
    saveData();
    res.json(fingerprints[userIndex]);
  });

  // DELETE a fingerprint user
  app.delete('/api/fingerprints/:id', requireAuth, (req, res) => {
    const id = parseInt(req.params.id);
    const initialLength = fingerprints.length;
    
    fingerprints = fingerprints.filter(fp => fp.id !== id);
    saveData();
    
    if (fingerprints.length === initialLength) {
      return res.status(404).json({ error: 'Fingerprint user not found' });
    }
    
    res.json({ message: 'Fingerprint user deleted successfully' });
  });

  // GET endpoint for ESP32 to retrieve fingerprint data
  app.get('/api/fingerprints/data', (req, res) => {
    // Format the response to be easy for ESP32 to parse
    const fingerprintsList = fingerprints.map(fp => ({
      fingerprintId: fp.fingerprintId
    }));
    res.json({ fingerprints: fingerprintsList });
  });

  // Process registration of new user with NFC and fingerprint
  app.post('/api/register-user', requireAuth, (req, res) => {
    const { cardUid, fingerprintId, userName } = req.body;
    
    if (!cardUid || !fingerprintId || !userName) {
      return res.status(400).json({ error: 'Card UID, fingerprint ID, and user name are required' });
    }
    
    // Find the card
    const card = nfcCards.find(c => c.uid === cardUid);
    if (!card) {
      return res.status(404).json({ error: 'Card not found' });
    }
    
    // Check if fingerprint already exists
    if (fingerprints.some(fp => fp.fingerprintId === fingerprintId)) {
      return res.status(409).json({ error: 'Fingerprint ID already exists' });
    }
    
    // Find the next available ID
    const nextId = fingerprints.length > 0 
      ? Math.max(...fingerprints.map(fp => fp.id)) + 1 
      : 1;
    
    // Create new user
    const newUser = {
      id: nextId,
      name: userName,
      fingerprintId: fingerprintId,
      registered: new Date().toISOString()
    };
    
    fingerprints.push(newUser);
    saveData();
    res.status(201).json(newUser);
  });

  // Process registration of new user with NFC and fingerprint from the ESP32 device
  app.post('/api/register-fingerprint', (req, res) => {
    const { cardUid, fingerprintId, userName } = req.body;
    
    if (!cardUid || !fingerprintId) {
      return res.status(400).json({ error: 'Card UID and fingerprint ID are required' });
    }
    
    // Find the card
    const card = nfcCards.find(c => c.uid === cardUid);
    if (!card) {
      return res.status(404).json({ error: 'Card not found' });
    }
    
    // Check if fingerprint already exists
    if (fingerprints.some(fp => fp.fingerprintId === fingerprintId)) {
      return res.status(409).json({ error: 'Fingerprint ID already exists' });
    }
    
    // Find the next available ID
    const nextId = fingerprints.length > 0 
      ? Math.max(...fingerprints.map(fp => fp.id)) + 1 
      : 1;
    
    // Use a default name if not provided
    const displayName = userName || `User ${nextId}`;
    
    // Create new user
    const newUser = {
      id: nextId,
      name: displayName,
      fingerprintId: fingerprintId,
      registered: new Date().toISOString()
    };
    
    fingerprints.push(newUser);
    saveData();
    
    console.log(`New user registered: ${displayName} with fingerprint ID ${fingerprintId}`);
    
    res.status(201).json(newUser);
  });
}

function setupBluetoothRoutes() {
  // GET all bluetooth MACs
  app.get('/api/bluetooth', requireAuth, (req, res) => {
    res.json(btMacs);
  });

  // POST a new bluetooth MAC
  app.post('/api/bluetooth', requireAuth, (req, res) => {
    const { mac, name } = req.body;
    
    if (!mac || !name) {
      return res.status(400).json({ error: 'Bluetooth MAC and name are required' });
    }
    
    // Check if MAC already exists
    if (btMacs.some(device => device.mac === mac)) {
      return res.status(409).json({ error: 'Bluetooth MAC already exists' });
    }
    
    const newDevice = { mac, name };
    btMacs.push(newDevice);
    saveData();
    res.status(201).json(newDevice);
  });

  // DELETE a bluetooth MAC
  app.delete('/api/bluetooth/:mac', requireAuth, (req, res) => {
    const macAddress = req.params.mac;
    const initialLength = btMacs.length;
    
    btMacs = btMacs.filter(device => device.mac !== macAddress);
    saveData();
    
    if (btMacs.length === initialLength) {
      return res.status(404).json({ error: 'Bluetooth MAC not found' });
    }
    
    res.json({ message: 'Bluetooth MAC deleted successfully' });
  });

  // GET endpoint for ESP32 to retrieve all bluetooth MACs
  app.get('/api/bluetooth/macs', (req, res) => {
    // Format the response to be easy for ESP32 to parse
    const macList = btMacs.map(device => ({ mac: device.mac, name: device.name }));
    res.json({ devices: macList });
  });
}

function setupSecurityLogRoutes() {
  // GET all security logs
  app.get('/api/security-logs', requireAuth, (req, res) => {
    // Optional filtering by type
    const { type } = req.query;
    
    if (type && Object.values(LogType).includes(type)) {
      const filteredLogs = securityLogs.filter(log => log.type === type);
      return res.json(filteredLogs);
    }
    
    res.json(securityLogs);
  });

  // GET a single log's photo if it exists
  app.get('/api/security-logs/photos/:filename', requireAuth, (req, res) => {
    const filename = req.params.filename;
    const photoPath = path.join(PHOTOS_DIR, filename);
    
    if (fs.existsSync(photoPath)) {
      res.sendFile(photoPath);
    } else {
      res.status(404).json({ error: 'Photo not found' });
    }
  });

  // POST a new security log (from ESP32 or admin)
  app.post('/api/security-logs', (req, res) => {
    let { type, description, deviceId, timestamp } = req.body;
    
    // Validate log type
    if (!type || !Object.values(LogType).includes(type)) {
      return res.status(400).json({ error: 'Valid log type is required' });
    }
    
    // Create a new log entry
    const newLog = {
      id: Date.now().toString(), // Simple unique ID
      type,
      description: description || 'No description provided',
      deviceId: deviceId || 'unknown',
      timestamp: timestamp || new Date().toISOString(),
      photoFilename: null
    };
    
    // Add to logs and save
    securityLogs.push(newLog);
    saveData();
    
    // If this is an intrusion attempt that includes a photo
    if (req.files && req.files.photo && type === LogType.ACCESS_DENIED) {
      const photo = req.files.photo;
      const photoFilename = `${newLog.id}.jpg`;
      const photoPath = path.join(PHOTOS_DIR, photoFilename);
      
      // Save the photo
      photo.mv(photoPath, (err) => {
        if (err) {
          console.error('Error saving photo:', err);
          return res.status(500).json({ error: 'Failed to save photo', log: newLog });
        }
        
        // Update log with photo filename
        newLog.photoFilename = photoFilename;
        saveData();
        
        res.status(201).json(newLog);
      });
    } else {
      res.status(201).json(newLog);
    }
  });

  // POST endpoint for ESP32 to upload photo with intrusion data
  app.post('/api/security-logs/upload-photo', (req, res) => {
    const { type, description, deviceId } = req.body;
    
    if (!req.files || !req.files.photo) {
      return res.status(400).json({ error: 'No photo uploaded' });
    }
    
    // Generate a unique filename using timestamp
    const timestamp = Date.now();
    const photoFilename = `${timestamp}.jpg`;
    const photoPath = path.join(PHOTOS_DIR, photoFilename);
    
    // Create a new log entry
    const newLog = {
      id: timestamp.toString(),
      type: type || LogType.ACCESS_DENIED,
      description: description || 'Unauthorized access attempt',
      deviceId: deviceId || 'unknown',
      timestamp: new Date().toISOString(),
      photoFilename: photoFilename
    };
    
    // Save the photo
    req.files.photo.mv(photoPath, (err) => {
      if (err) {
        console.error('Error saving photo:', err);
        return res.status(500).json({ error: 'Failed to save photo' });
      }
      
      // Add log and save data
      securityLogs.push(newLog);
      saveData();
      
      res.status(201).json(newLog);
    });
  });

  // DELETE a specific security log
  app.delete('/api/security-logs/:id', requireAuth, (req, res) => {
    const logId = req.params.id;
    
    // Find the log to get photo filename if it exists
    const logToDelete = securityLogs.find(log => log.id === logId);
    
    if (!logToDelete) {
      return res.status(404).json({ error: 'Security log not found' });
    }
    
    // Delete the photo if it exists
    if (logToDelete.photoFilename) {
      const photoPath = path.join(PHOTOS_DIR, logToDelete.photoFilename);
      if (fs.existsSync(photoPath)) {
        try {
          fs.unlinkSync(photoPath);
        } catch (error) {
          console.error('Error deleting photo:', error);
        }
      }
    }
    
    // Remove from logs array
    securityLogs = securityLogs.filter(log => log.id !== logId);
    saveData();
    
    res.json({ message: 'Security log deleted successfully' });
  });

  // DELETE all security logs
  app.delete('/api/security-logs', requireAuth, (req, res) => {
    // Optionally filter by type
    const { type } = req.query;
    
    if (type && Object.values(LogType).includes(type)) {
      // Find logs of this type with photos
      const logsToDelete = securityLogs.filter(log => log.type === type && log.photoFilename);
      
      // Delete associated photos
      logsToDelete.forEach(log => {
        if (log.photoFilename) {
          const photoPath = path.join(PHOTOS_DIR, log.photoFilename);
          if (fs.existsSync(photoPath)) {
            try {
              fs.unlinkSync(photoPath);
            } catch (error) {
              console.error(`Error deleting photo ${log.photoFilename}:`, error);
            }
          }
        }
      });
      
      // Remove logs of this type
      securityLogs = securityLogs.filter(log => log.type !== type);
    } else {
      // Delete all photos
      securityLogs.forEach(log => {
        if (log.photoFilename) {
          const photoPath = path.join(PHOTOS_DIR, log.photoFilename);
          if (fs.existsSync(photoPath)) {
            try {
              fs.unlinkSync(photoPath);
            } catch (error) {
              console.error(`Error deleting photo ${log.photoFilename}:`, error);
            }
          }
        }
      });
      
      // Clear all logs
      securityLogs = [];
    }
    
    saveData();
    res.json({ message: 'Security logs deleted successfully' });
  });
}

function setupSystemRoutes() {
  // GET system status
  app.get('/api/status', requireAuth, (req, res) => {
    updateSystemStatus();
    res.json(systemStatus);
  });

  // ESP32 check-in endpoint
  app.post('/api/checkin', (req, res) => {
    const { deviceId, ip } = req.body;
    
    if (!deviceId) {
      return res.status(400).json({ error: 'Device ID is required' });
    }
    
    // Update or add device to active ESP32s
    const existingDeviceIndex = systemStatus.activeESP32s.findIndex(dev => dev.deviceId === deviceId);
    
    if (existingDeviceIndex >= 0) {
      systemStatus.activeESP32s[existingDeviceIndex].lastSeen = new Date().toISOString();
      systemStatus.activeESP32s[existingDeviceIndex].ip = ip || 'unknown';
    } else {
      systemStatus.activeESP32s.push({
        deviceId,
        ip: ip || 'unknown',
        firstSeen: new Date().toISOString(),
        lastSeen: new Date().toISOString()
      });
    }
    
    updateSystemStatus();
    res.json({ status: 'success', timestamp: systemStatus.lastSync });
  });
}
