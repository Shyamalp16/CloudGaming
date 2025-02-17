// Import RTCPeerConnection from the wrtc package
const { RTCPeerConnection } = require('wrtc');
const WebSocket = require('ws');

const serverUrl = 'ws://localhost:3000';
let ws;
let peerConnection;

// ICE config: specify STUN servers
const config = {
  iceServers: [{ urls: 'stun:stun.l.google.com:19302' }]
};

function connectToSignalingServer() {
  ws = new WebSocket(serverUrl);

  ws.onopen = () => {
    console.log('Connected to signaling server');
  };

  ws.onmessage = async (event) => {
    const msg = JSON.parse(event.data);
    console.log('Received from server:', msg);

    switch (msg.type) {
      case 'answer':
        await handleAnswer(msg);
        break;
      case 'ice-candidate':
        await handleRemoteIceCandidate(msg.candidate);
        break;
      case 'offer':
        console.warn('Unexpected offer received: this client is the offerer.');
        break;
      default:
        console.warn('Unknown message type:', msg.type);
    }
  };

  ws.onerror = (err) => {
    console.error('WebSocket error:', err);
  };
}

function createPeerConnection() {
  peerConnection = new RTCPeerConnection(config);

  peerConnection.onicecandidate = (event) => {
    if (event.candidate) {
      console.log('Local ICE candidate:', event.candidate);
      ws.send(JSON.stringify({
        type: 'ice-candidate',
        candidate: event.candidate
      }));
    }
  };

  peerConnection.ondatachannel = (event) => {
    const receiveChannel = event.channel;
    console.log('Data channel received:', receiveChannel.label);
  };

  // Optional: for media streams, use peerConnection.ontrack
}

async function handleAnswer(answerMsg) {
  const remoteDesc = new RTCSessionDescription({ type: 'answer', sdp: answerMsg.sdp });
  await peerConnection.setRemoteDescription(remoteDesc);
  console.log('Remote description set with answer');
}

async function handleRemoteIceCandidate(candidate) {
  try {
    await peerConnection.addIceCandidate(candidate);
    console.log('Added remote ICE candidate:', candidate);
  } catch (err) {
    console.error('Error adding received ICE candidate:', err);
  }
}

async function startConnection() {
  if (!peerConnection) createPeerConnection();

  // Create a data channel (optional)
  const dataChannel = peerConnection.createDataChannel('chat');
  dataChannel.onopen = () => {
    console.log('Data channel opened');
    dataChannel.send('Hello from the offerer!');
  };
  dataChannel.onmessage = (event) => {
    console.log('Received message on data channel:', event.data);
  };

  const offer = await peerConnection.createOffer();
  await peerConnection.setLocalDescription(offer);

  ws.send(JSON.stringify({
    type: 'offer',
    sdp: offer.sdp
  }));
  console.log('Offer sent:', offer.sdp);
}

connectToSignalingServer();

// For example, start connection automatically after a delay or on some trigger
setTimeout(() => {
  startConnection();
}, 3000);
