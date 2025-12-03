# DisplayCaptureProject - High-Level Architecture

## 1. System Overview

```mermaid
graph LR
    A[Gaming PC<br/>Host] --> B[Signaling Server<br/>Node.js + Redis]
    B --> C[Web Client<br/>Browser]
    C -.->|Direct P2P Stream| A
    
    style A fill:#ffebee
    style B fill:#e3f2fd  
    style C fill:#e8f5e8
```

## 2. Data Flow

```mermaid
sequenceDiagram
    participant Client as Web Client
    participant Server as Signaling Server  
    participant Host as Gaming PC
    
    Note over Client,Host: 1. Connect
    Client->>Server: Join Room
    Server->>Host: Connect to Room
    Host->>Client: Establish P2P Connection
    
    Note over Client,Host: 2. Stream
    loop Real-time Streaming
        Host->>Client: Video + Audio Stream
        Client->>Host: Keyboard + Mouse Input
    end
```

## 3. Connection Process

```mermaid
graph TD
    A[Client Opens Browser] --> B[Connect to Server]
    B --> C[Host Joins Room]
    C --> D[Direct P2P Connection]
    D --> E[Start Streaming]
    
    style A fill:#e1f5fe
    style E fill:#c8e6c9
    style D fill:#fff3e0
```

## 4. Quality Adaptation

```mermaid
graph LR
    A[Client] --> B[Network Feedback]
    B --> C[Host Adjusts Quality]
    C --> D[Better Stream]
    D --> A
    
    style B fill:#fff3e0
    style C fill:#e8f5e8
```

## 5. Key Components

```mermaid
graph TB
    subgraph "Gaming PC"
        A[Game Capture] --> B[Video Encoder]
        C[Audio Capture] --> D[Audio Encoder]
        E[Input Handler] --> F[Input Injection]
    end
    
    subgraph "Cloud Server"
        G[Signaling Server] --> H[Room Management]
    end
    
    subgraph "Web Client"
        I[Video Player] --> J[Input Controls]
    end
    
    B --> I
    D --> I
    J --> E
    G -.->|Connection Setup| A
    G -.->|Connection Setup| I
```

## 6. Deployment

```mermaid
graph TB
    A[Gaming PC<br/>Windows] --> B[Cloud Server<br/>Node.js + Redis]
    B --> C[Web Browser<br/>Any Device]
    C -.->|Direct Stream| A
    
    style A fill:#ffebee
    style B fill:#e3f2fd
    style C fill:#e8f5e8
```

## Key Features

### 🚀 **Performance**
- Ultra-low latency streaming (<40ms)
- High frame rate support (up to 144 FPS)
- Hardware-accelerated encoding
- Adaptive quality control

### 🎮 **Gaming Focus**
- Real-time input injection
- Direct game process targeting
- Professional audio capture
- Network resilience

### 🌐 **Scalability**
- Redis-based room management
- WebRTC P2P connections
- Cross-platform web clients
- Cloud-ready architecture
