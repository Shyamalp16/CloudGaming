const WebSocket = require('ws')
const { RTCPeerConnection } = require('wrtc')
const server = new WebSocket.Server({port : 3000})

//array of max 2 peers
let connections = []

server.on('connection', (ws) => {

  ws.on('error', (error) => {
    console.error('Websocket Error: ', error)
  })


  if(connections.length >=2){
    console.log('2 Peers Conneceted, Refusing Any New Connections')
    try{
      ws.close(1000, 'Maximum Connections Reached')
    }catch(error){
      console.error('Error Closing Extra Connection', error)
    }
    return
  }

  connections.push(ws)
  console.log('New Client Connected, Total Connections:', connections.length)

  //make a RTCPeerConnection object as a client (game player)
  const peerConnection = new RTCPeerConnection({
    iceServers: [{urls: 'stun:stun.l.google.com:19302'}]
  })
  ws.peerConnection = peerConnection

  //when ice are collected, send it thrugh websocket signaling channel
  peerConnection.onicecandidate = (event) => {
    if(event.candidate){
      console.log('Local ICE Candidate: ', event.candidate)
      const candidateMessage = {
        type: 'ice-candidate',
        candidate: event.candidate
      }
      ws.send(JSON.stringify(candidateMessage))
    }
  }

  // client is the first offerer, so when the CLIENT joins ws, create offer and save as local desc
  if(connections.length === 1) {
    peerConnection.createOffer().then(offer => {
      console.log('Created Offer:', offer)
      return peerConnection.setLocalDescription(offer)
    }).then(() => {
      //send offer (SDP) over to the host (game host)
      const offerMsg = {
        type: 'offer',
        sdp: peerConnection.localDescription.sdp
      }
      console.log('Sending Offer: ', offerMsg)
      ws.send(JSON.stringify(offerMsg))
    }).catch(error => {
      console.error('Error During Offer Creation:', error)
    })
  }

  // try{
  //   ws.send(JSON.stringify({type : 'offer', data : 'NodeJS Hello Signal'}), (error) => {
  //     if(error){
  //       console.error('Error Sending Hello Message From NodeJS:', error)
  //     }
  //   })
  // }catch(error){
  //   console.error('Exception Sending Hello Message From NodeJS:', error)
  // }

  ws.on('message', (message) => {
    console.log('Received Message:', message);
    let parsed;

    try{
      parsed = JSON.parse(message)
    }catch(error){
      console.error('Failed To Parse Message:', error)
      return;
    }

    switch(parsed.type){
      case 'offer':
        console.log('Received Offer:', message);
        forwardToOther(ws, parsed);
        break;

      case 'answer':
        console.log('Received Answer:', message);
        forwardToOther(ws, parsed);
        break;

      case 'ice-candidate':
        console.log('Received ICE-Candidate:', message);
        forwardToOther(ws, parsed);
        if(ws.peerConnection && parsed.candidate){
          ws.peerConnection.addIceCandidate(parsed.candidate).catch(error => console.error('Error Adding ICE Candidate:', error))
        }
        break;

      default:
        console.log('Unknown Message Type:', parsed.type);
        break;
    }
  })

  ws.on('close', () => {
    console.log('Client Disconnected')
    connections = connections.filter((client) => client != ws)
  })
})


//server level error handling
server.on('error', (error) => {
  console.error('Server Error:', error)
})


console.log('SignalingServer Listening on Port 3000')

function forwardToOther(sender, message){
  const data = JSON.stringify(message)
  const otherPeer = connections.find((client) => client != sender)

  if(otherPeer && otherPeer.readyState == WebSocket.OPEN){
    try{
      otherPeer.send(data, (error) => {
        if(error){
          console.error('Error Forwarding Message To Other Peer:', error)
        }
      })
    }catch(error){
      console.error('Exception While Forwarding Message:', error)
    }
  }else{
    console.warn('No Available Peer To Send The Message To')
  }
}