#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <iostream>
#include <iomanip>
#include <cstdint>


const int BUFFER_SIZE = 4096;
const int MAGIC_NUMBER_LENGTH = 6;
const char PROTOCOL[] =
"\xc1\xa1\x10"
"ftp";

struct Header {
    char m_protocol[MAGIC_NUMBER_LENGTH]; /* protocol magic number (6 bytes) */
    uint8_t m_type;                       /* type (1 byte) */
    uint8_t m_status;                     /* status (1 byte) */
    uint32_t m_length;                    /* length (4 bytes) in Big endian*/

    Header(uint8_t mt, uint8_t ms, uint32_t ml) {
        strncpy(m_protocol, PROTOCOL, MAGIC_NUMBER_LENGTH);
        m_type = mt;
        m_status = ms;
        m_length = htonl(ml);
    }
    Header() {
        strncpy(m_protocol, PROTOCOL, MAGIC_NUMBER_LENGTH);
        m_type = 0;
        m_status = 0;
        m_length = 0;
    }
} __attribute__((packed));


// before using safesend you should define buf size
ssize_t safesend(const int& sockfd, const void* buf, size_t length, int flags) {
    const char* buffer = static_cast<const char*>(buf);
    size_t totalSent = 0;
    while (totalSent < length) {
        ssize_t currentSent = send(sockfd, buffer + totalSent, length - totalSent, flags);

        if (currentSent == -1) {
            perror("Error in safesend: unexpected.");
            return -1;
        }
        if (currentSent == 0) {
            perror("Error in safesend: socket closed.");
            return -1;
        }
        totalSent += static_cast<size_t>(currentSent);
    }
    return totalSent;
}

ssize_t saferecv(const int& sockfd, void* buf, size_t length, int flags) {
    char* buffer = static_cast<char*>(buf);
    size_t totalReceived = 0;
    while (totalReceived < length) {
        ssize_t currentReceived = recv(sockfd, buffer + totalReceived, length - totalReceived, flags);
        if (currentReceived == -1) {
            perror("Error in saferecv: unexpected.");
            return -1;
        }
        if (currentReceived == 0) {
            // This indicates that the remote side has closed the connection
            perror("Error in saferecv: socket closed.");
            return -1;
        }
        totalReceived += static_cast<size_t>(currentReceived);
    }
    return totalReceived;
}





int openConnection(const std::string& ip, int port) {
    int clientSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (clientSocket == -1) {
        perror("Socket creation failed");
        return -1;
    }


    struct sockaddr_in serverAddr;
    bzero(&serverAddr, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &serverAddr.sin_addr) <= 0) {
        perror("Invalid IP address");
        close(clientSocket);
        return -1;
    }

    if (connect(clientSocket, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        perror("Connection failed");
        close(clientSocket);
        return -1;
    }

    // Prepare and send the OPEN_CONN_REQUEST message
    Header openRequest(0xA1, 0, 12);

    send(clientSocket, &openRequest, sizeof(openRequest), 0);

    // Receive and process the OPEN_CONN_REPLY message
    Header openReply;

    recv(clientSocket, &openReply, sizeof(openReply), 0);

    // memcmp(openReply.m_protocol, PROTOCOL, MAGIC_NUMBER_LENGTH) == 0 &&
    if (openReply.m_type == 0xA2 && openReply.m_status == 1) {
        std::cout << "Connection opened successfully." << std::endl;
    }
    else {
        std::cerr << "Unexpected response from the server. Failed to open the connection." << std::endl;
    }

    return clientSocket;
}

void ls(int clientSocket) {
    Header lsRequest(0xA3, 0, 12);
    send(clientSocket, &lsRequest, sizeof(lsRequest), 0);

    // Receive and process the LIST_REPLY header
    Header lsReply;
    recv(clientSocket, &lsReply, sizeof(lsReply), 0);
    // std::cout << "ls recevied m_type";
    // std::cout << "0x" << std::setfill('0') << std::setw(2) << std::hex << static_cast<int>(lsReply.m_type) << std::endl;

    if (lsReply.m_type != 0xA4) {
        std::cerr << "Unexpected response from the server. Failed to get ls." << std::endl;
        return;
    }

    char buffer[BUFFER_SIZE];
    ssize_t lsPayload = recv(clientSocket, buffer, sizeof(buffer), 0);

    if (lsPayload < 0) {
        perror("Error reading from socket");
        return;
    }

    std::cout << buffer;
}

void put(int clientSocket, const std::string& filename) {
    std::ifstream file(filename, std::ios::in | std::ios::binary);
    if (!file) {
        std::cerr << "Failed to open file: " << filename << ". Does it really exist?" << std::endl;
        return;
    }

    // Get the file size
    file.seekg(0, std::ios::end);
    size_t fileSize = file.tellg();
    file.seekg(0, std::ios::beg);

    // Prepare and send the PUT_REQUEST message
    Header putRequest(0xA7, 0, 12 + filename.length() + 1);
    send(clientSocket, &putRequest, sizeof(putRequest), 0);

    // Send the filename as payload
    send(clientSocket, filename.c_str(), filename.length() + 1, 0);

    // Receive and process the PUT_REPLY message
    Header putReply;
    recv(clientSocket, &putReply, sizeof(putReply), 0);

    // || putReply.m_length != 12
    if (putReply.m_type != 0xA8) {
        std::cerr << "Unexpected response from the server. Failed to put file." << std::endl;
        return;
    }

    // Prepare and send the FILE_DATA message
    Header dataHeader(0xFF, 0, 12 + fileSize);
    send(clientSocket, &dataHeader, sizeof(dataHeader), 0);

    // Read and send the file content
    // safe send
    char buffer[BUFFER_SIZE];
    size_t totalBytesUploaded = 0;
    while (totalBytesUploaded < fileSize) {
        size_t bytesRead = file.read(buffer, sizeof(buffer)).gcount();
        send(clientSocket, buffer + totalBytesUploaded, bytesRead, 0);
        totalBytesUploaded += bytesRead;
    }
}

void get(int clientSocket, const std::string& filename) {
    // Send GET_REQUEST message to request the file
    Header getRequest(0xA5, 0, 12 + filename.length() + 1);
    send(clientSocket, &getRequest, sizeof(getRequest), 0);

    // Send the filename as payload
    send(clientSocket, filename.c_str(), filename.length() + 1, 0);

    // Receive and process the GET_REPLY message
    Header getReply;
    recv(clientSocket, &getReply, sizeof(getReply), 0);

    //  || getReply.m_length != 12
    if (getReply.m_type != 0xA6) {
        std::cerr << "Unexpected response from the server. Failed getReply from server." << std::endl;
        return;
    }

    if (getReply.m_status == 0) {
        std::cerr << "The requested file does not exist on the server." << std::endl;
        return;
    }

    // The server is ready to send the file, so receive FILE_DATA
    Header dataHeader;
    recv(clientSocket, &dataHeader, sizeof(dataHeader), 0);

    if (dataHeader.m_type != 0xFF) {
        std::cerr << "Unexpected response from the server. Failed to getFile from server." << std::endl;
        return;
    }

    // Create or open the local file for writing
    std::ofstream outFile(filename, std::ios::out | std::ios::binary);
    if (!outFile) {
        std::cerr << "Failed to create or open the local file." << std::endl;
        return;
    }

    // Receive and write the file content
    char buffer[BUFFER_SIZE];
    size_t totalBytesReceived = 0;
    size_t fileSize = ntohl(dataHeader.m_length) - 12;

    // safe recv
    while (totalBytesReceived < fileSize) {
        ssize_t bytesRead = recv(clientSocket, buffer, sizeof(buffer), 0);
        if (bytesRead < 0) {
            perror("Error reading from socket");
            outFile.close();
            return;
        }
        outFile.write(buffer, bytesRead);
        totalBytesReceived += bytesRead;
    }

    outFile.close();

    std::cout << "File: " << filename << " downloaded successfully." << std::endl;
}

void sha256(int clientSocket, const std::string& filename) {
    // Send SHA_REQUEST message to request the checksum
    Header shaRequest(0xA9, 0, 12 + filename.length() + 1);
    send(clientSocket, &shaRequest, sizeof(shaRequest), 0);

    // Send the filename as payload
    send(clientSocket, filename.c_str(), filename.length() + 1, 0);

    // Receive and process the SHA_REPLY message
    Header shaReply;
    recv(clientSocket, &shaReply, sizeof(shaReply), 0);

    // || shaReply.m_length != 12
    if (shaReply.m_type != 0xAA) {
        std::cerr << "Unexpected response from the server. Failed shaReply from server." << std::endl;
        return;
    }

    if (shaReply.m_status == 0) {
        std::cerr << "The requested file does not exist on the server." << std::endl;
        return;
    }

    // The server is ready to send the checksum, so receive FILE_DATA
    Header dataHeader;
    recv(clientSocket, &dataHeader, sizeof(dataHeader), 0);

    if (dataHeader.m_type != 0xFF) {
        std::cerr << "Unexpected response from the server. Failed get sha256 from server." << std::endl;
        return;
    }

    char checksumBuffer[BUFFER_SIZE];
    recv(clientSocket, checksumBuffer, sizeof(checksumBuffer), 0);

    // Ensure that the checksum string is null-terminated
    checksumBuffer[BUFFER_SIZE - 1] = '\0';

    std::cout << "SHA256 Checksum of " << filename << ": " << checksumBuffer << std::endl;
}

void quit(int clientSocket) {
    if (clientSocket != -1) {
        // Send QUIT_REQUEST message to request to close the connection
        Header quitRequest(0xAB, 0, 12);
        send(clientSocket, &quitRequest, sizeof(quitRequest), 0);

        // Receive and process the QUIT_REPLY message
        Header quitReply;
        recv(clientSocket, &quitReply, sizeof(quitReply), 0);

        if (quitReply.m_type != 0xAC) {
            std::cerr << "Unexpected response from the server. Failed to close the connection." << std::endl;
        }

        // Close the client socket
        close(clientSocket);

        // You may add additional cleanup or exit logic here if needed
        // ...
        return;
    }

    // Exit the client program
    exit(EXIT_FAILURE);
}

void parseline(int& clientSocket) { // passing & clientSocket here
    std::string inputLine;

    std::getline(std::cin, inputLine);

    if (inputLine.empty()) {
        return;  // Ignore empty lines
    }

    std::istringstream iss(inputLine);
    std::string command;
    iss >> command;

    if (command == "open") {

        std::string ip;
        int port;
        iss >> ip >> port;
        if (clientSocket != -1) {
            std::cerr << "Before opening, current " << clientSocket << " was closed" << std::endl;
            close(clientSocket);
        }
        clientSocket = openConnection(ip, port);
        return;
        // clientSocket = openConnection(ip, port); make no effect to the actuall clientsocket
    }
    if (command == "ls" && clientSocket != -1) {
        ls(clientSocket);
        return;
    }
    if (command == "get") {
        std::string filename;
        iss >> filename;
        get(clientSocket, filename);
        return;
    }
    if (command == "put") {
        std::string filename;
        iss >> filename;
        put(clientSocket, filename);
        return;
    }
    if (command == "sha256") {
        std::string filename;
        iss >> filename;
        sha256(clientSocket, filename);
        return;
    }
    if (command == "quit") {
        quit(clientSocket);
        return;
    }

    std::cerr << "Unexpected command from input. Invalid command or incorrect usage." << std::endl;

    return;
}

int main() {
    int clientSocket = -1;
    std::cerr << "hello from ftp client" << std::endl;
    while (true) {
        parseline(clientSocket);
    }

    return 0;
}
