const WebSocket = require('ws')
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

  try{
    ws.send(JSON.stringify({type : 'offer', data : 'NodeJS Hello Signal'}), (error) => {
      if(error){
        console.error('Error Sending Hello Message From NodeJS:', error)
      }
    })
  }catch(error){
    console.error('Exception Sending Hello Message From NodeJS:', error)
  }

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