const express = require('express')
const http = require('http')
const socketIO = require('socket.io')

const app = express()
const server = http.createServer(app)
const io = socketIO(server)

app.use(express.static('public'))

io.on('connection', socket => {
    console.log('New client connected: ', socket.id);
  
    // When a peer sends an offer
    socket.on('offer', offer => {
      console.log('Received offer:', offer);
      socket.broadcast.emit('offer', offer);
    });
  
    // When a peer sends an answer.
    socket.on('answer', answer => {
      console.log('Received answer:', answer);
      socket.broadcast.emit('answer', answer);
    });
  
    // When a peer sends an ICE candidate.
    socket.on('ice-candidate', candidate => {
      console.log('Received ICE candidate:', candidate);
      socket.broadcast.emit('ice-candidate', candidate);
    });
  
    socket.on('disconnect', () => {
      console.log('Client disconnected: ', socket.id);
    });
});

server.listen(3000, () => {
    console.log("Signaling server is listening on port 3000")
})