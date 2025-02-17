const { RTCPeerConnection } = require('wrtc');
const WebSocket = require('ws');

const serverUrl = 'ws://localhost:3000';
let ws;
let peerConnection;

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
        console.warn('Unexpected Offer Received, This Client Will Always Be The Offerer.');
        break;
      default:
        console.warn('Unknown Message Type:', msg.type);
    }
  };

  ws.onerror = (err) => {
    console.error('WebSocket Error:', err);
  };
}

function createPeerConnection() {
  peerConnection = new RTCPeerConnection(config);

  peerConnection.onicecandidate = (event) => {
    if (event.candidate) {
      console.log('Local ICE Candidate:', event.candidate);
      ws.send(JSON.stringify({
        type: 'ice-candidate',
        candidate: event.candidate
      }));
    }
  };

  peerConnection.ondatachannel = (event) => {
    const receiveChannel = event.channel;
    console.log('Data Channel Received:', receiveChannel.label);
  };
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

  const dataChannel = peerConnection.createDataChannel('tst');
  dataChannel.onopen = () => {
    console.log('Data Channel Opened');
    dataChannel.send('Hello From The Offerer!');
  };
  dataChannel.onmessage = (event) => {
    console.log('Received Message On Data Channel:', event.data);
  };

  const offer = await peerConnection.createOffer();
  await peerConnection.setLocalDescription(offer);

  ws.send(JSON.stringify({
    type: 'offer',
    sdp: offer.sdp
  }));
  console.log('Offer Sent:', offer.sdp);
}

connectToSignalingServer();

setTimeout(() => {
  startConnection();
}, 3000);
