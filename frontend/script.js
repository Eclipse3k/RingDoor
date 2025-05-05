// API and UI Management for ESP32 Access Control System

// Función para obtener la URL base del API automáticamente
function getApiBaseUrl() {
  // Obtener hostname actual (dirección IP o localhost)
  const hostname = window.location.hostname;
  const port = 3000; // Puerto fijo del servidor
  return `http://${hostname}:${port}/api`;
}

const API_URL = getApiBaseUrl();

// ===== Core API Functions =====

// Generic fetch with authentication handling
async function fetchWithAuth(url, options = {}) {
  try {
    const response = await fetch(url, options);
    
    if (response.status === 401) {
      // Authentication error - redirect to login
      window.location.href = '/login.html';
      return null;
    }
    
    return response;
  } catch (error) {
    console.error('Fetch error:', error);
    throw error;
  }
}

// Generic data fetcher for various endpoints
async function fetchData(endpoint, errorMessage = 'Failed to load data') {
  try {
    const response = await fetchWithAuth(`${API_URL}/${endpoint}`);
    if (!response) return null; // Auth error handled by fetchWithAuth
    
    return await response.json();
  } catch (error) {
    console.error(`Error fetching ${endpoint}:`, error);
    showNotification(errorMessage, 'error');
    return null;
  }
}

// Generic API post function
async function postData(endpoint, data, successMessage = 'Data added successfully') {
  try {
    const response = await fetchWithAuth(`${API_URL}/${endpoint}`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(data)
    });
    
    if (!response) return null; // Auth error handled by fetchWithAuth
    
    if (!response.ok) {
      const error = await response.json();
      throw new Error(error.error || 'Operation failed');
    }
    
    showNotification(successMessage, 'success');
    return await response.json();
  } catch (error) {
    console.error(`Error posting to ${endpoint}:`, error);
    showNotification(error.message, 'error');
    return null;
  }
}

// Generic API update function
async function updateData(endpoint, id, data, successMessage = 'Data updated successfully') {
  try {
    const response = await fetchWithAuth(`${API_URL}/${endpoint}/${id}`, {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(data)
    });
    
    if (!response) return null; // Auth error handled by fetchWithAuth
    
    if (!response.ok) {
      const error = await response.json();
      throw new Error(error.error || 'Operation failed');
    }
    
    showNotification(successMessage, 'success');
    return await response.json();
  } catch (error) {
    console.error(`Error updating ${endpoint}:`, error);
    showNotification(error.message, 'error');
    return null;
  }
}

// Generic API delete function
async function deleteData(endpoint, id, successMessage = 'Data deleted successfully') {
  try {
    const response = await fetchWithAuth(`${API_URL}/${endpoint}/${id}`, {
      method: 'DELETE'
    });
    if (!response) return false; // Auth error handled by fetchWithAuth
    if (!response.ok) {
      const error = await response.json();
      throw new Error(error.error || 'Operation failed');
    }
    showNotification(successMessage, 'success');
    return true;
  } catch (error) {
    console.error(`Error deleting from ${endpoint}:`, error);
    showNotification(error.message, 'error');
    return false;
  }
}

// Store what to delete
let pendingDelete = null;

// Unified delete confirmation modal for all types
function showDeleteConfirmation(idsOrObj) {
  const deleteModal = document.getElementById('deleteModal');
  // idsOrObj can be [logId], [cardId], [userId], [mac], or an object {type, id}
  pendingDelete = idsOrObj;
  // Update confirmation message
  const confirmationMessage = document.querySelector('#deleteModal .modal-body p');
  if (Array.isArray(idsOrObj) && idsOrObj.length === 1 && typeof idsOrObj[0] === 'string') {
    confirmationMessage.textContent = 'Are you sure you want to delete this item?';
  } else if (Array.isArray(idsOrObj) && idsOrObj.length === 1) {
    confirmationMessage.textContent = 'Are you sure you want to delete this log?';
  } else if (Array.isArray(idsOrObj) && idsOrObj.length === 0) {
    confirmationMessage.textContent = 'Are you sure you want to delete all selected logs?';
  } else if (typeof idsOrObj === 'object' && idsOrObj.type) {
    confirmationMessage.textContent = `Are you sure you want to delete this ${idsOrObj.type}?`;
  } else {
    confirmationMessage.textContent = 'Are you sure you want to delete this item?';
  }
  deleteModal.classList.add('show');
  document.addEventListener('keydown', closeModalOnEscape);
}

// Confirm delete handler
async function confirmDeleteHandler() {
  const deleteModal = document.getElementById('deleteModal');
  if (!pendingDelete) return;
  // Handle logs (array of ids)
  if (Array.isArray(pendingDelete)) {
    await deleteLogs(pendingDelete);
    closeDeleteModal();
    pendingDelete = null;
    return;
  }
  // Handle object {type, id}
  if (typeof pendingDelete === 'object' && pendingDelete.type && pendingDelete.id) {
    if (pendingDelete.type === 'card') {
      await deleteData('cards', pendingDelete.id, 'Card deleted successfully');
      fetchCards();
    } else if (pendingDelete.type === 'user') {
      await deleteData('fingerprints', pendingDelete.id, 'User deleted successfully');
      fetchFingerprints();
    } else if (pendingDelete.type === 'bluetooth') {
      await deleteData('bluetooth', pendingDelete.id, 'Bluetooth device deleted successfully');
      fetchBtDevices();
    }
    closeDeleteModal();
    pendingDelete = null;
    return;
  }
  closeDeleteModal();
  pendingDelete = null;
}

// ===== Data Management Functions =====

// NFC Card functions
async function fetchCards() {
  const cards = await fetchData('cards', 'Failed to load cards. Please check if the server is running.');
  if (cards) {
    displayCards(cards);
    updateCardDropdown(cards);
  }
  updateSyncTime();
  return cards;
}

async function addCard(e) {
  e.preventDefault();
  
  const cardIdInput = document.getElementById('cardId');
  const cardNameInput = document.getElementById('cardName');
  const uid = cardIdInput.value.trim().toUpperCase();
  
  // Validate Card UID format (hexadecimal)
  const hexRegex = /^[0-9A-F]+$/;
  if (!hexRegex.test(uid)) {
    showNotification('Please enter a valid card UID in hexadecimal format (e.g., 04A21B6F)', 'error');
    return;
  }
  
  const newCard = {
    uid: uid,
    name: cardNameInput.value || `Card ${uid.substring(0, 6)}`
  };
  
  const result = await postData('cards', newCard, 'NFC Card added successfully');
  if (result) {
    document.getElementById('addCardForm').reset();
    fetchCards();
  }
}

async function handleDeleteCard(e) {
  const cardUid = e.target.closest('button').dataset.uid;
  showDeleteConfirmation({type: 'card', id: cardUid});
}

async function saveCardChanges(uid) {
  const name = document.getElementById('editCardName').value;
  const result = await updateData('cards', uid, { name }, 'Card updated successfully');
  
  if (result) {
    document.getElementById('editCardModal').classList.remove('show');
    fetchCards();
  }
}

// Fingerprint functions
async function fetchFingerprints() {
  const fingerprints = await fetchData('fingerprints', 'Failed to load fingerprint users. Please check if the server is running.');
  if (fingerprints) {
    displayFingerprints(fingerprints);
  }
  updateSyncTime();
  return fingerprints;
}

async function addFingerprint(e) {
  e.preventDefault();
  
  const nameInput = document.getElementById('fingerprintName');
  const idInput = document.getElementById('fingerprintId');
  
  const fingerprintId = idInput.value.trim();
  const name = nameInput.value.trim();
  
  if (!name || !fingerprintId) {
    showNotification('Name and fingerprint ID are required', 'error');
    return;
  }
  
  const result = await postData('fingerprints', { name, fingerprintId }, 'Fingerprint user added successfully');
  if (result) {
    document.getElementById('addFingerprintForm').reset();
    fetchFingerprints();
  }
}

async function handleDeleteFingerprint(e) {
  const id = e.target.closest('button').dataset.id;
  showDeleteConfirmation({type: 'user', id: id});
}

async function saveFingerprintChanges(id) {
  const name = document.getElementById('editFingerprintName').value;
  const result = await updateData('fingerprints', id, { name }, 'User updated successfully');
  
  if (result) {
    document.getElementById('editFingerprintModal').classList.remove('show');
    fetchFingerprints();
  }
}

async function registerUser(e) {
  e.preventDefault();
  
  const cardUid = document.getElementById('cardSelect').value;
  const fingerprintId = document.getElementById('registerFingerprintId').value.trim();
  const userName = document.getElementById('registerUserName').value.trim();
  
  if (!cardUid || !fingerprintId || !userName) {
    showNotification('All fields are required', 'error');
    return;
  }
  
  const result = await postData('register-user', { cardUid, fingerprintId, userName }, 'User registered successfully');
  if (result) {
    document.getElementById('registerUserForm').reset();
    fetchFingerprints();
  }
}

// Bluetooth functions
async function fetchBtDevices() {
  const devices = await fetchData('bluetooth', 'Failed to load bluetooth devices. Please check if the server is running.');
  if (devices) {
    displayBtDevices(devices);
  }
  updateSyncTime();
  return devices;
}

async function addBtDevice(e) {
  e.preventDefault();
  
  const btMacInput = document.getElementById('btMac');
  const btNameInput = document.getElementById('btName');
  const mac = btMacInput.value;
  const name = btNameInput.value;
  
  // Validate MAC address format
  if (!isValidMacAddress(mac)) {
    showNotification('Please enter a valid MAC address in format XX:XX:XX:XX:XX:XX', 'error');
    return;
  }
  
  const result = await postData('bluetooth', { mac: mac.toUpperCase(), name }, 'Bluetooth device added successfully');
  if (result) {
    document.getElementById('addBtForm').reset();
    fetchBtDevices();
  }
}

async function handleDeleteBtDevice(e) {
  const macAddress = e.target.closest('button').dataset.mac;
  showDeleteConfirmation({type: 'bluetooth', id: macAddress});
}

// Security Logs functions
async function fetchSecurityLogs(filter = 'all') {
  try {
    let url = `security-logs`;
    if (filter !== 'all') {
      url += `?type=${filter}`;
    }
    
    const logs = await fetchData(url, 'Failed to load security logs. Please check if the server is running.');
    if (logs) {
      displaySecurityLogs(logs);
    }
  } catch (error) {
    console.error('Error fetching security logs:', error);
    showNotification('Failed to load security logs. Please check if the server is running.', 'error');
  }
}

async function deleteLogs(logIds) {
  try {
    // If single log, use DELETE on specific log
    if (logIds.length === 1) {
      if (await deleteData('security-logs', logIds[0], 'Log deleted successfully')) {
        fetchSecurityLogs(currentLogFilter);
      }
    } 
    // If all logs, use DELETE on all logs
    else if (logIds.length === 0) {
      let url = `security-logs`;
      if (currentLogFilter !== 'all') {
        url += `?type=${currentLogFilter}`;
      }
      
      const response = await fetchWithAuth(`${API_URL}/${url}`, {
        method: 'DELETE'
      });
      
      if (!response) return; // Auth error handled by fetchWithAuth
      
      showNotification('Logs deleted successfully', 'success');
      fetchSecurityLogs(currentLogFilter);
    }
  } catch (error) {
    console.error('Error deleting logs:', error);
    showNotification('Failed to delete logs', 'error');
  }
}

// System status
async function fetchSystemStatus() {
  try {
    const status = await fetchData('status');
    if (!status) return;
    
    // Update status display
    updateStatusDisplay(status);
  } catch (error) {
    console.error('Error fetching system status:', error);
  }
}

function updateStatusDisplay(status) {
  const lastSyncTimeElement = document.getElementById('lastSyncTime');
  const activeDevicesListElement = document.getElementById('activeDevicesList');
  const cardCountElement = document.getElementById('cardCount');
  const userCountElement = document.getElementById('userCount');
  const btCountElement = document.getElementById('btCount');
  const securityLogsCountElement = document.getElementById('securityLogsCount');
  
  // Update status display
  lastSyncTimeElement.textContent = new Date(status.lastSync).toLocaleString();
  cardCountElement.textContent = status.totalCards;
  userCountElement.textContent = status.totalUsers;
  btCountElement.textContent = status.totalBtDevices;
  securityLogsCountElement.textContent = status.totalSecurityLogs;
  
  // Update active devices list with enhanced status indicators
  if (status.activeESP32s.length > 0) {
    activeDevicesListElement.innerHTML = '';
    status.activeESP32s.forEach(device => {
      // Calculate time since last seen for status indication
      const lastSeen = new Date(device.lastSeen);
      const now = new Date();
      const minutesSinceLastSeen = Math.floor((now - lastSeen) / (1000 * 60));
      
      // Determine connection status
      let connectionStatus = 'connected';
      if (minutesSinceLastSeen > 10) {
        connectionStatus = 'disconnected';
      } else if (minutesSinceLastSeen > 2) {
        connectionStatus = 'idle';
      }
      
      const li = document.createElement('li');
      li.className = 'device-item';
      li.innerHTML = `
        <span class="device-id">${device.deviceId}</span>
        <span class="device-ip ${connectionStatus}">${device.ip}</span>
        <span class="device-lastseen">${formatTimeSince(lastSeen)}</span>
      `;
      activeDevicesListElement.appendChild(li);
    });
  } else {
    activeDevicesListElement.innerHTML = '<li class="device-item">No devices connected</li>';
  }
}

// ===== UI Display Functions =====

// Display cards in the table
function displayCards(cards) {
  const cardTableBody = document.getElementById('cardTableBody');
  cardTableBody.innerHTML = '';
  
  cards.forEach(card => {
    const row = document.createElement('tr');
    
    // Format name to use either custom name or default name based on UID
    const displayName = card.name || `Card ${card.uid.substring(0, 6)}`;
    
    row.innerHTML = `
      <td>${card.uid}</td>
      <td>${displayName}</td>
      <td>
        <button class="edit-btn" data-uid="${card.uid}" data-name="${displayName}">
          <i class="fas fa-edit"></i>
        </button>
        <button class="delete-btn" data-uid="${card.uid}">
          <i class="fas fa-trash-alt"></i>
        </button>
      </td>
    `;
    
    cardTableBody.appendChild(row);
  });
  
  // Add event listeners to buttons
  document.querySelectorAll('button.edit-btn').forEach(button => {
    button.addEventListener('click', handleEditCard);
  });
  
  document.querySelectorAll('button.delete-btn').forEach(button => {
    button.addEventListener('click', handleDeleteCard);
  });
}

// Display fingerprint users in the table
function displayFingerprints(fingerprints) {
  const fingerprintTableBody = document.getElementById('fingerprintTableBody');
  fingerprintTableBody.innerHTML = '';
  
  fingerprints.forEach(fp => {
    const row = document.createElement('tr');
    const registeredDate = new Date(fp.registered).toLocaleDateString();
    
    row.innerHTML = `
      <td>${fp.name}</td>
      <td>${fp.fingerprintId}</td>
      <td>${registeredDate}</td>
      <td>
        <button class="edit-fp-btn" data-id="${fp.id}" data-name="${fp.name}">
          <i class="fas fa-edit"></i>
        </button>
        <button class="delete-fp-btn" data-id="${fp.id}">
          <i class="fas fa-trash-alt"></i>
        </button>
      </td>
    `;
    
    fingerprintTableBody.appendChild(row);
  });
  
  // Add event listeners to buttons
  document.querySelectorAll('button.edit-fp-btn').forEach(button => {
    button.addEventListener('click', handleEditFingerprint);
  });
  
  document.querySelectorAll('button.delete-fp-btn').forEach(button => {
    button.addEventListener('click', handleDeleteFingerprint);
  });
}

// Display bluetooth devices in the table
function displayBtDevices(devices) {
  const btTableBody = document.getElementById('btTableBody');
  btTableBody.innerHTML = '';
  
  devices.forEach(device => {
    const row = document.createElement('tr');
    
    row.innerHTML = `
      <td>${device.mac}</td>
      <td>${device.name}</td>
      <td>
        <button class="delete-bt-btn" data-mac="${device.mac}">
          <i class="fas fa-trash-alt"></i>
        </button>
      </td>
    `;
    
    btTableBody.appendChild(row);
  });
  
  // Add event listeners to delete buttons
  document.querySelectorAll('button.delete-bt-btn').forEach(button => {
    button.addEventListener('click', handleDeleteBtDevice);
  });
}

// Update card dropdown for user registration
function updateCardDropdown(cards) {
  const cardSelect = document.getElementById('cardSelect');
  if (!cardSelect) return;
  
  cardSelect.innerHTML = '<option value="">Select NFC Card</option>';
  
  cards.forEach(card => {
    const option = document.createElement('option');
    option.value = card.uid;
    option.textContent = card.name || `Card ${card.uid.substring(0, 6)}`;
    cardSelect.appendChild(option);
  });
}

// Display security logs
function displaySecurityLogs(logs) {
  const logsWrapper = document.getElementById('logsWrapper');
  
  if (!logs || logs.length === 0) {
    logsWrapper.innerHTML = `
      <div class="log-empty-state">
        <i class="fas fa-clipboard-list"></i>
        <p>No security logs found</p>
      </div>
    `;
    return;
  }
  
  logsWrapper.innerHTML = '';
  
  // Sort logs by timestamp (newest first)
  logs.sort((a, b) => new Date(b.timestamp) - new Date(a.timestamp));
  
  logs.forEach(log => {
    // Determine log type styling and icon
    let logTypeClass = '';
    let logIcon = '';
    
    switch (log.type) {
      case 'access_denied':
        logTypeClass = 'access-denied';
        logIcon = '<i class="fas fa-ban"></i>';
        break;
      case 'motion_detected':
        logTypeClass = 'motion-detected';
        logIcon = '<i class="fas fa-walking"></i>';
        break;
      case 'access_granted':
        logTypeClass = 'access-granted';
        logIcon = '<i class="fas fa-check-circle"></i>';
        break;
      default:
        logIcon = '<i class="fas fa-info-circle"></i>';
    }
    
    // Format timestamp
    const timestamp = new Date(log.timestamp).toLocaleString();
    
    // Create log element
    const logElement = document.createElement('div');
    logElement.className = `security-log ${logTypeClass}`;
    logElement.setAttribute('data-log-id', log.id);
    
    // Add main log content
    logElement.innerHTML = `
      <div class="log-icon">
        ${logIcon}
      </div>
      <div class="log-content">
        <div class="log-header">
          <span class="log-timestamp">${timestamp}</span>
          <span class="log-device">Device: ${log.deviceId}</span>
        </div>
        <p class="log-description">${log.description}</p>
        <div class="log-actions">
          <button class="log-action-btn delete-log-btn" data-log-id="${log.id}">
            <i class="fas fa-trash-alt"></i>
            <span class="log-action-text">Delete</span>
          </button>
        </div>
      </div>
    `;
    
    // If log has photo, add view photo button
    if (log.photoFilename) {
      const actionsDiv = logElement.querySelector('.log-actions');
      const viewPhotoBtn = document.createElement('button');
      viewPhotoBtn.className = 'log-action-btn view-photo-btn';
      viewPhotoBtn.setAttribute('data-photo', log.photoFilename);
      viewPhotoBtn.setAttribute('data-timestamp', timestamp);
      viewPhotoBtn.setAttribute('data-description', log.description);
      viewPhotoBtn.innerHTML = `
        <i class="fas fa-camera"></i>
        <span class="log-action-text">View Photo</span>
      `;
      
      actionsDiv.prepend(viewPhotoBtn);
    }
    
    logsWrapper.appendChild(logElement);
  });
  
  // Add event listeners for view photo buttons
  document.querySelectorAll('.view-photo-btn').forEach(btn => {
    btn.addEventListener('click', () => {
      const photoFilename = btn.getAttribute('data-photo');
      const timestamp = btn.getAttribute('data-timestamp');
      const description = btn.getAttribute('data-description');
      
      openPhotoModal(photoFilename, timestamp, description);
    });
  });
  
  // Add event listeners for delete buttons
  document.querySelectorAll('.delete-log-btn').forEach(btn => {
    btn.addEventListener('click', () => {
      const logId = btn.getAttribute('data-log-id');
      showDeleteConfirmation([logId]);
    });
  });
}

// ===== UI Form Handlers =====

// Handle editing a card
function handleEditCard(e) {
  const button = e.currentTarget;
  const uid = button.dataset.uid;
  const name = button.dataset.name;
  
  // Open edit modal
  const modal = document.getElementById('editCardModal');
  document.getElementById('editCardUid').value = uid;
  document.getElementById('editCardName').value = name;
  
  // Show the modal
  modal.classList.add('show');
  
  // Prevent any bubbling that might close the modal immediately
  e.stopPropagation();
}

// Handle editing a fingerprint user
function handleEditFingerprint(e) {
  const button = e.currentTarget;
  const id = button.dataset.id;
  const name = button.dataset.name;
  
  // Open edit modal
  const modal = document.getElementById('editFingerprintModal');
  document.getElementById('editFingerprintId').value = id;
  document.getElementById('editFingerprintName').value = name;
  
  modal.classList.add('show');
}

// Open photo modal
function openPhotoModal(photoFilename, timestamp, description) {
  const securityPhoto = document.getElementById('securityPhoto');
  const photoTimestamp = document.getElementById('photoTimestamp');
  const photoDescription = document.getElementById('photoDescription');
  const photoModal = document.getElementById('photoModal');
  
  securityPhoto.src = `${API_URL}/security-logs/photos/${photoFilename}`;
  photoTimestamp.textContent = `Time: ${timestamp}`;
  photoDescription.textContent = `Description: ${description}`;
  
  photoModal.classList.add('show');
  
  // Add event listener for escape key
  document.addEventListener('keydown', closeModalOnEscape);
}

// Close photo modal
function closePhotoModal() {
  const photoModal = document.getElementById('photoModal');
  const securityPhoto = document.getElementById('securityPhoto');
  
  photoModal.classList.remove('show');
  
  // Remove event listener for escape key
  document.removeEventListener('keydown', closeModalOnEscape);
  
  // Clear photo src after animation
  setTimeout(() => {
    securityPhoto.src = '';
  }, 300);
}

// Close delete confirmation modal
function closeDeleteModal() {
  const deleteModal = document.getElementById('deleteModal');
  deleteModal.classList.remove('show');
  
  // Remove event listener for escape key
  document.removeEventListener('keydown', closeModalOnEscape);
  
  // Clear logs to delete
  logsToDelete = [];
}

// Close modal when escape key is pressed
function closeModalOnEscape(e) {
  if (e.key === 'Escape') {
    closePhotoModal();
    closeDeleteModal();
  }
}

// ===== Utility Functions =====

// Show notification message
function showNotification(message, type = 'info') {
  const notification = document.createElement('div');
  notification.className = `notification ${type}`;
  notification.innerHTML = `
    <span>${message}</span>
    <button class="close-notification">×</button>
  `;
  
  document.body.appendChild(notification);
  
  // Add close button functionality
  notification.querySelector('.close-notification').addEventListener('click', function() {
    document.body.removeChild(notification);
  });
  
  // Auto-hide after 5 seconds
  setTimeout(() => {
    if (document.body.contains(notification)) {
      document.body.removeChild(notification);
    }
  }, 5000);
}

// Update the sync time display
function updateSyncTime() {
  const lastSyncTimeElement = document.getElementById('lastSyncTime');
  const now = new Date();
  lastSyncTimeElement.textContent = now.toLocaleString();
  
  // Also update system status
  fetchSystemStatus();
}

// Format time since in a human-readable format
function formatTimeSince(date) {
  const seconds = Math.floor((new Date() - date) / 1000);
  
  let interval = Math.floor(seconds / 31536000);
  if (interval > 1) return `Last seen: ${interval} years ago`;
  if (interval === 1) return `Last seen: 1 year ago`;
  
  interval = Math.floor(seconds / 2592000);
  if (interval > 1) return `Last seen: ${interval} months ago`;
  if (interval === 1) return `Last seen: 1 month ago`;
  
  interval = Math.floor(seconds / 86400);
  if (interval > 1) return `Last seen: ${interval} days ago`;
  if (interval === 1) return `Last seen: 1 day ago`;
  
  interval = Math.floor(seconds / 3600);
  if (interval > 1) return `Last seen: ${interval} hours ago`;
  if (interval === 1) return `Last seen: 1 hour ago`;
  
  interval = Math.floor(seconds / 60);
  if (interval > 1) return `Last seen: ${interval} minutes ago`;
  if (interval === 1) return `Last seen: 1 minute ago`;
  
  if (seconds < 10) return 'Last seen: just now';
  
  return `Last seen: ${Math.floor(seconds)} seconds ago`;
}

// Validate MAC address format
function isValidMacAddress(mac) {
  // Regular expression to match MAC address format XX:XX:XX:XX:XX:XX
  const macRegex = /^([0-9A-Fa-f]{2}[:-]){5}([0-9A-Fa-f]{2})$/;
  return macRegex.test(mac);
}

// Handle tab switching
function setupTabs() {
  const tabs = document.querySelectorAll('.tab-button');
  const tabContents = document.querySelectorAll('.tab-content');
  
  tabs.forEach(tab => {
    tab.addEventListener('click', () => {
      // Remove active class from all tabs and contents
      tabs.forEach(t => t.classList.remove('active'));
      tabContents.forEach(content => content.classList.remove('active'));
      
      // Add active class to clicked tab and corresponding content
      tab.classList.add('active');
      const contentId = tab.dataset.tab;
      document.getElementById(contentId).classList.add('active');
    });
  });
}

// Add logout functionality
function setupLogout() {
  const logoutButton = document.getElementById('logoutButton');
  if (logoutButton) {
    logoutButton.addEventListener('click', () => {
      window.location.href = '/api/logout';
    });
  }
}

// ===== Polling for new users and logs =====
let lastUserIds = new Set();
let lastLogIds = new Set();

async function pollForNewUsersAndLogs() {
  // Poll users
  const fingerprints = await fetchData('fingerprints');
  if (fingerprints) {
    const currentUserIds = new Set(fingerprints.map(u => u.id));
    // Detect new users
    if (lastUserIds.size > 0) {
      fingerprints.forEach(u => {
        if (!lastUserIds.has(u.id)) {
          showNotification(`New user registered: ${u.name}`, 'success');
        }
      });
    }
    lastUserIds = currentUserIds;
    displayFingerprints(fingerprints);
  }

  // Poll logs - respetando el filtro seleccionado
  let url = 'security-logs';
  if (currentLogFilter !== 'all') {
    url += `?type=${currentLogFilter}`;
  }
  
  const logs = await fetchData(url);
  if (logs) {
    const currentLogIds = new Set(logs.map(l => l.id));
    // Detect new logs
    if (lastLogIds.size > 0) {
      logs.forEach(l => {
        if (!lastLogIds.has(l.id)) {
          showNotification(`New security log: ${l.description}`, 'info');
        }
      });
    }
    lastLogIds = currentLogIds;
    displaySecurityLogs(logs);
  }
}

// Start polling every 5 seconds
setInterval(pollForNewUsersAndLogs, 5000);

// ===== Event Initialization =====

// Current logs filter and logs to be deleted
let currentLogFilter = 'all';
let logsToDelete = [];

// Initialize all event listeners
function initEventListeners() {
  // Form event listeners
  const addCardForm = document.getElementById('addCardForm');
  const addFingerprintForm = document.getElementById('addFingerprintForm');
  const registerUserForm = document.getElementById('registerUserForm');
  const addBtForm = document.getElementById('addBtForm');
  const filterButtons = document.querySelectorAll('.filter-btn');
  const deleteAllLogsBtn = document.getElementById('deleteAllLogsBtn');
  
  if (addCardForm) addCardForm.addEventListener('submit', addCard);
  if (addFingerprintForm) addFingerprintForm.addEventListener('submit', addFingerprint);
  if (registerUserForm) registerUserForm.addEventListener('submit', registerUser);
  if (addBtForm) addBtForm.addEventListener('submit', addBtDevice);
  
  // Log filter button listeners
  if (filterButtons) {
    filterButtons.forEach(btn => {
      btn.addEventListener('click', () => {
        filterButtons.forEach(b => b.classList.remove('active'));
        btn.classList.add('active');
        currentLogFilter = btn.dataset.filter;
        fetchSecurityLogs(currentLogFilter);
      });
    });
  }
  
  // Delete all logs button
  if (deleteAllLogsBtn) {
    deleteAllLogsBtn.addEventListener('click', () => {
      showDeleteConfirmation([]);
    });
  }
  
  // Cancel and confirm delete buttons
  const cancelDeleteBtn = document.getElementById('cancelDeleteBtn');
  const confirmDeleteBtn = document.getElementById('confirmDeleteBtn');
  
  if (cancelDeleteBtn) {
    cancelDeleteBtn.addEventListener('click', closeDeleteModal);
  }
  
  if (confirmDeleteBtn) {
    confirmDeleteBtn.addEventListener('click', confirmDeleteHandler);
  }
  
  // Close buttons for modals
  const closePhotoModalBtn = document.querySelector('#photoModal .close-modal');
  if (closePhotoModalBtn) {
    closePhotoModalBtn.addEventListener('click', closePhotoModal);
  }
  
  const closeEditCardModalBtn = document.querySelector('#editCardModal .close-modal');
  if (closeEditCardModalBtn) {
    closeEditCardModalBtn.addEventListener('click', () => {
      document.getElementById('editCardModal').classList.remove('show');
    });
  }
  
  const cancelEditCardBtns = document.querySelectorAll('.cancel-card-edit');
  if (cancelEditCardBtns.length > 0) {
    cancelEditCardBtns.forEach(btn => {
      btn.addEventListener('click', () => {
        document.getElementById('editCardModal').classList.remove('show');
      });
    });
  }
  
  const closeEditFingerprintModalBtn = document.querySelector('#editFingerprintModal .close-modal');
  if (closeEditFingerprintModalBtn) {
    closeEditFingerprintModalBtn.addEventListener('click', () => {
      document.getElementById('editFingerprintModal').classList.remove('show');
    });
  }
  
  const cancelEditFingerprintBtns = document.querySelectorAll('.cancel-fingerprint-edit');
  if (cancelEditFingerprintBtns.length > 0) {
    cancelEditFingerprintBtns.forEach(btn => {
      btn.addEventListener('click', () => {
        document.getElementById('editFingerprintModal').classList.remove('show');
      });
    });
  }
  
  // Save changes buttons
  const saveCardBtn = document.getElementById('saveCardChangesBtn');
  if (saveCardBtn) {
    saveCardBtn.addEventListener('click', () => {
      const uid = document.getElementById('editCardUid').value;
      saveCardChanges(uid);
    });
  }
  
  const saveFingerprintBtn = document.getElementById('saveFingerprintChangesBtn');
  if (saveFingerprintBtn) {
    saveFingerprintBtn.addEventListener('click', () => {
      const id = document.getElementById('editFingerprintId').value;
      saveFingerprintChanges(id);
    });
  }
}

// Initialize the app
document.addEventListener('DOMContentLoaded', () => {
  // Setup UI
  setupTabs();
  setupLogout();
  
  // Load data
  fetchCards();
  fetchFingerprints();
  fetchBtDevices();
  fetchSystemStatus();
  fetchSecurityLogs('all');
  
  // Initialize event listeners
  initEventListeners();
  
  // Enable closing modals by clicking outside modal-content
  document.querySelectorAll('.modal').forEach(modal => {
    modal.addEventListener('mousedown', function(e) {
      if (e.target === modal) {
        modal.classList.remove('show');
        // Special handling: clear photo src if photo modal
        if (modal.id === 'photoModal') {
          setTimeout(() => {
            const securityPhoto = document.getElementById('securityPhoto');
            if (securityPhoto) securityPhoto.src = '';
          }, 300);
        }
        // Special handling: clear logsToDelete for delete modal
        if (modal.id === 'deleteModal') {
          logsToDelete = [];
        }
      }
    });
  });

  // Ensure all close-modal X buttons close their modal
  document.querySelectorAll('.modal .close-modal').forEach(btn => {
    btn.addEventListener('click', function() {
      const modal = btn.closest('.modal');
      if (modal) {
        modal.classList.remove('show');
        // Special handling: clear photo src if photo modal
        if (modal.id === 'photoModal') {
          setTimeout(() => {
            const securityPhoto = document.getElementById('securityPhoto');
            if (securityPhoto) securityPhoto.src = '';
          }, 300);
        }
        // Special handling: clear logsToDelete for delete modal
        if (modal.id === 'deleteModal') {
          logsToDelete = [];
        }
      }
    });
  });
    // Set up periodic updates
  setInterval(fetchSystemStatus, 30000); // Every 30 seconds
  
  // En lugar de actualizar los logs directamente, usemos la función de polling que ya respeta el filtro
  // Eliminamos esta línea: setInterval(() => fetchSecurityLogs(currentLogFilter), 30000);
  // El polling ya actualiza los logs cada 5 segundos
});
