const { RTCPeerConnection, RTCSessionDescription, RTCRtpReceiver } = require('wrtc');
const WebSocket = require('ws');

const serverUrl = 'ws://localhost:3000';
let ws;
let peerConnection;

const config = {
  iceServers: [{ urls: 'stun:stun.l.google.com:19302' }],
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

  peerConnection.ontrack = (event) => {
    console.log('Received remote track:', event.track.kind, event.track.id);
    if (event.track.kind === 'video') {
      const { createWriteStream } = require('fs');
      const stream = new MediaStream([event.track]);
      const writer = createWriteStream('output.h264');
      event.track.ondataavailable = (event) => {
        writer.write(Buffer.from(event.data));
      };
      event.track.onended = () => {
        writer.end();
        console.log('Video stream saved to output.h264');
      };
    }
  };

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
      receiveChannel.send('Hello From The Receiver!');
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

  peerConnection.addTransceiver('video', {
    direction: 'recvonly',
    streams: [],
  });

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
    // Modify SDP to include H.264 while preserving existing codecs
    let sdpLines = offer.sdp.split('\r\n');
    let videoLineIndex = -1;
    for (let i = 0; i < sdpLines.length; i++) {
      if (sdpLines[i].startsWith('m=video')) {
        videoLineIndex = i;
        break;
      }
    }
    if (videoLineIndex !== -1) {
      // Add H.264 to the existing codec list (e.g., append 96)
      let videoLine = sdpLines[videoLineIndex];
      if (!videoLine.includes(' 96')) {
        sdpLines[videoLineIndex] = videoLine.replace(/(SAVPF.*)$/, '$1 96');
      }
      // Find the last rtpmap line to insert H.264 definition
      let lastRtpmapIndex = videoLineIndex;
      for (let i = videoLineIndex + 1; i < sdpLines.length; i++) {
        if (sdpLines[i].startsWith('a=rtpmap:')) {
          lastRtpmapIndex = i;
        } else if (sdpLines[i].startsWith('m=')) {
          break;
        }
      }
      // Add H.264 codec definition and feedback mechanisms
      sdpLines.splice(lastRtpmapIndex + 1, 0,
        'a=rtpmap:96 H264/90000',
        'a=fmtp:96 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f',
        'a=rtcp-fb:96 nack',
        'a=rtcp-fb:96 nack pli'
      );
    }
    offer.sdp = sdpLines.join('\r\n');
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