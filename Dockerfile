# Stage 1: Build the application
FROM ubuntu:22.04 AS builder

# Install build dependencies
RUN apt-get update && \
    apt-get install -y g++ make && \
    rm -rf /var/lib/apt/lists/*

# Set working directory
WORKDIR /app

# Copy all source files
COPY . .

# Build the docker-specific binary
RUN make docker

# Stage 2: Create a minimal runtime image
FROM ubuntu:22.04

# Set working directory
WORKDIR /app

# Copy only the compiled binary from the builder stage
COPY --from=builder /app/session_engine_docker /app/session_engine_docker

# Create logs directory
RUN mkdir -p /app/logs

# Expose the UDP port the load balancer listens on
EXPOSE 9000/udp

# Run the load balancer server
CMD ["./session_engine_docker"]
