const { RTCPeerConnection, RTCSessionDescription } = require('wrtc');
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
    try {
      const msg = JSON.parse(event.data);
      console.log('Received from server:', JSON.stringify(msg, null, 2));

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
    } catch (err) {
      console.error('Error processing WebSocket message:', err);
    }
  };

  ws.onerror = (err) => {
    console.error('WebSocket Error:', err);
  };

  ws.onclose = () => {
    console.log('WebSocket connection closed');
  };
}

function createPeerConnection() {
  peerConnection = new RTCPeerConnection(config);

  peerConnection.onicecandidate = (event) => {
    if (event.candidate) {
      console.log('Local ICE Candidate:', JSON.stringify(event.candidate, null, 2));
      ws.send(JSON.stringify({
        type: 'ice-candidate',
        candidate: event.candidate
      }));
    } else {
      console.log('ICE candidate gathering complete');
    }
  };

  peerConnection.oniceconnectionstatechange = () => {
    console.log('ICE connection state changed to:', peerConnection.iceConnectionState);
  };

  peerConnection.onicegatheringstatechange = () => {
    console.log('ICE gathering state changed to:', peerConnection.iceGatheringState);
  };

  peerConnection.onconnectionstatechange = () => {
    console.log('PeerConnection state changed to:', peerConnection.connectionState);
  };

  peerConnection.onsignalingstatechange = () => {
    console.log('Signaling state changed to:', peerConnection.signalingState);
  };

  peerConnection.ondatachannel = (event) => {
    const receiveChannel = event.channel;
    console.log('Data Channel Received:', receiveChannel.label);

    receiveChannel.onopen = () => {
      console.log('Data Channel Opened:', receiveChannel.label);
    };
    receiveChannel.onmessage = (event) => {
      console.log('Received Message on Data Channel:', event.data);
    };
    receiveChannel.onclose = () => {
      console.log('Data Channel Closed:', receiveChannel.label);
    };
    receiveChannel.onerror = (err) => {
      console.error('Data Channel Error:', err);
    };
  };
}

async function handleAnswer(answerMsg) {
  console.log('Received SDP in answer:', JSON.stringify(answerMsg, null, 2));
  if (!answerMsg.sdp || typeof answerMsg.sdp !== 'string' || !answerMsg.sdp.startsWith('v=')) {
    console.error('Invalid or missing SDP in answer:', answerMsg);
    return;
  }
  try {
    const remoteDesc = new RTCSessionDescription({ type: 'answer', sdp: answerMsg.sdp });
    await peerConnection.setRemoteDescription(remoteDesc);
    console.log('Remote description set with answer');
  } catch (err) {
    console.error('Error setting remote description:', err);
  }
}

async function handleRemoteIceCandidate(candidate) {
  try {
    await peerConnection.addIceCandidate(candidate);
    console.log('Added remote ICE candidate:', JSON.stringify(candidate, null, 2));
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
  dataChannel.onclose = () => {
    console.log('Data Channel Closed');
  };
  dataChannel.onerror = (err) => {
    console.error('Data Channel Error:', err);
  };

  try {
    const offer = await peerConnection.createOffer();
    await peerConnection.setLocalDescription(offer);
    ws.send(JSON.stringify({
      type: 'offer',
      sdp: offer.sdp
    }));
    console.log('Offer Sent:', offer.sdp);
  } catch (err) {
    console.error('Error creating or setting offer:', err);
  }
}

connectToSignalingServer();

setTimeout(() => {
  startConnection();
}, 3000);