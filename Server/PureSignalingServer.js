const WebSocket = require('ws')
const server = new WebSocket.Server({port : 3000})

//array of max 2 peer connection
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

    ws.on('message', (message) => {
        let parsed
        try{
            parsed = JSON.parse(message)
        }catch(error){
            console.error('Failed To Parse Message:', error)
            return;
        }
        forwardToOther(ws, parsed)
    })

    ws.on('close', () => {
        console.log('Client Disconnected');
        const otherPeer = connections.find(client => client !== ws);
        connections = connections.filter(client => client !== ws);

        if (otherPeer && otherPeer.readyState === WebSocket.OPEN) {
            try {
                otherPeer.send(JSON.stringify({ type: 'peer-disconnected' }), (error) => {
                    if (error) {
                        console.error('Error notifying peer of disconnection:', error);
                    }
                });
            } catch (error) {
                console.error('Exception while notifying peer of disconnection:', error);
            }
        }
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
