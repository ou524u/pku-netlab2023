#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <limits.h>

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

int createServerSocket(const char* ip, int port) {
    int serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket == -1) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);

    if (inet_pton(AF_INET, ip, &serverAddr.sin_addr) <= 0) {
        perror("Invalid IP address");
        exit(EXIT_FAILURE);
    }

    if (bind(serverSocket, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        perror("Binding failed");
        exit(EXIT_FAILURE);
    }

    if (listen(serverSocket, 5) < 0) {
        perror("Listening failed");
        exit(EXIT_FAILURE);
    }

    return serverSocket;
}

int acceptClientConnection(int serverSocket) {
    struct sockaddr_in clientAddr;
    socklen_t clientAddrLen = sizeof(clientAddr);

    int clientSocket = accept(serverSocket, (struct sockaddr*)&clientAddr, &clientAddrLen);
    if (clientSocket == -1) {
        perror("Failed to accept client connection");
    }

    return clientSocket;
}

std::string calculateLS() {
    FILE* cmdOutput = popen("ls", "r");

    if (cmdOutput == nullptr) {
        std::cerr << "Failed to execute 'ls' command." << std::endl;
        // Send an empty file list as the payload
        return "";
    }

    // Read and send the output of the "ls" command as payload
    char buffer[BUFFER_SIZE];
    std::string fileList = "";

    while (fgets(buffer, sizeof(buffer), cmdOutput) != nullptr) {
        fileList += buffer;
    }
    pclose(cmdOutput);
    return fileList;

}

void listFiles(int clientSocket) {
    // Prepare and send the LIST_REPLY message
    Header lsReply(0xA4, 0, 12);

    // Read and send the output of the "ls" command as payload
    std::string fileList = calculateLS();

    lsReply.m_length = htonl(12 + fileList.size() + 1);
    send(clientSocket, &lsReply, sizeof(lsReply), 0);
    send(clientSocket, fileList.c_str(), fileList.size() + 1, 0);

}




void getFile(int clientSocket, const std::string& resourceDirectory) {


    // Check if the file exists
    std::ifstream file(resourceDirectory, std::ios::in | std::ios::binary);
    if (!file) {
        // File does not exist, send GET_REPLY with m_status = 0
        Header getReply(0xA6, 0, 12);
        send(clientSocket, &getReply, sizeof(getReply), 0);
        return;
    }

    // File exists, send GET_REPLY with m_status = 1
    Header getReply(0xA6, 1, 12);
    send(clientSocket, &getReply, sizeof(getReply), 0);

    // Send the file content in a FILE_DATA message
    file.seekg(0, std::ios::end);
    size_t fileSize = file.tellg();
    file.seekg(0, std::ios::beg);

    // Prepare and send the FILE_DATA message
    Header dataHeader(0xFF, 0, 12 + fileSize);
    send(clientSocket, &dataHeader, sizeof(dataHeader), 0);

    // Read and send the file content
    char buffer[BUFFER_SIZE];
    while (!file.eof()) {
        file.read(buffer, sizeof(buffer));
        send(clientSocket, buffer, file.gcount(), 0);
    }

    std::cout << "File: " << " sent successfully." << std::endl;
}

void putFile(int clientSocket, const std::string& filename) {
    // Check if the file already exists in the server's working directory
    std::ifstream fileExistsTest(filename);
    if (fileExistsTest) {
        std::cout << "File already exists in the server, and would be overwritten." << std::endl;
    }
    fileExistsTest.close();

    // Send a PUT_REPLY with status 1 to indicate that the server is ready to receive the file
    Header putReply(0xA8, 0, 12);
    send(clientSocket, &putReply, sizeof(putReply), 0);

    // Create the file in the server's working directory
    std::ofstream outputFile(filename, std::ios::out | std::ios::binary);
    if (!outputFile) {
        // Failed to create the file
        std::cerr << "Failed to create the file in the server directory. Upload failed." << std::endl;
        return;
    }

    // Receive the file content in chunks and write it to the server file
    Header dataHeader;
    recv(clientSocket, &dataHeader, sizeof(dataHeader), 0);
    if (dataHeader.m_type != 0xFF) {
        std::cerr << "Unexpected response from the client. Failed to receive uploaded file." << std::endl;
    }

    // safe recv
    char buffer[BUFFER_SIZE];
    size_t totalBytesReceived = 0;
    size_t totalBytestoUpload = ntohl(dataHeader.m_length) - 12;

    while (totalBytesReceived < totalBytestoUpload) {
        ssize_t bytesRead = recv(clientSocket, buffer, sizeof(buffer), 0);
        if (bytesRead < 0) {
            perror("Error reading from socket");
            outputFile.close();
            return;
        }

        outputFile.write(buffer, bytesRead);
        totalBytesReceived += bytesRead;
    }

    // Close the file
    outputFile.close();

    std::cout << "File: " << filename << " uploaded successfully." << std::endl;
}
std::string calculateSHA256(const std::string& filename) {
    std::string command = "sha256sum " + filename;
    FILE* cmdOutput = popen(command.c_str(), "r");

    if (cmdOutput == nullptr) {
        std::cerr << "Failed to execute 'sha256sum' command." << std::endl;
        return "";
    }

    char buffer[BUFFER_SIZE];
    std::string sha256sum = "";

    while (fgets(buffer, sizeof(buffer), cmdOutput) != nullptr) {
        sha256sum += buffer;
    }

    pclose(cmdOutput);

    // // Extract the SHA-256 checksum from the output
    // size_t spacePos = sha256sum.find_first_of(" ");
    // if (spacePos != std::string::npos) {
    //     sha256sum = sha256sum.substr(0, spacePos);
    // }

    return sha256sum;
}

void sha256File(int clientSocket, const std::string& filename) {
    // Check if the file exists in the server's working directory
    std::ifstream file(filename, std::ios::in | std::ios::binary);

    if (!file) {
        // File does not exist; send SHA_REPLY with m_status = 0
        Header shaReply(0xAA, 0, 12);
        send(clientSocket, &shaReply, sizeof(shaReply), 0);
        return;
    }
    file.close();

    // File exists; send SHA_REPLY with m_status = 1
    Header shaReply(0xAA, 1, 12);
    send(clientSocket, &shaReply, sizeof(shaReply), 0);

    // Calculate SHA-256 checksum
    std::string sha256sum = calculateSHA256(filename);

    // Send the SHA-256 checksum in a FILE_DATA message
    Header dataHeader(0xFF, 0, 12 + sha256sum.length() + 1);
    send(clientSocket, &dataHeader, sizeof(dataHeader), 0);
    send(clientSocket, sha256sum.c_str(), sha256sum.length() + 1, 0);

}




bool handleCommands(int clientSocket) {
    // Receive and process the command
    Header commandHeader;
    ssize_t bytesRead = recv(clientSocket, &commandHeader, sizeof(commandHeader), 0);

    if (bytesRead < 0) {
        perror("Error reading from socket");
        return false;
    }

    if (bytesRead == 0) {
        // Client closed the connection? Really?
        return true;
    }
    if (commandHeader.m_type == 0xA1) {  // open
        // reply for client open
        Header openReply(0xA2, 1, 12);
        send(clientSocket, &openReply, sizeof(openReply), 0);
        std::cout << "Open from client" << std::endl;
        return false;
    }

    if (commandHeader.m_type == 0xA3) {  // ls
        // Send the list of files to the client
        listFiles(clientSocket);
        std::cout << "listFile operated for client" << std::endl;
        return false;
    }
    if (commandHeader.m_type == 0xA5) {  // get
        // Send the requested file to the client
        char currentDir[PATH_MAX];  // PATH_MAX is a constant for maximum path length
        if (getcwd(currentDir, sizeof(currentDir)) == nullptr) {
            perror("Failed to get the current working directory");
            return false;
        }
        // Extract the filename from the payload
        char filenameBuffer[ntohl(commandHeader.m_length) - 12];
        recv(clientSocket, filenameBuffer, sizeof(filenameBuffer), 0);

        // Construct the full path to the requested file
        std::string resourceDirectory = std::string(currentDir) + "/" + std::string(filenameBuffer);

        getFile(clientSocket, resourceDirectory);
        std::cout << "getFile operated for client" << std::endl;
        return false;
    }
    if (commandHeader.m_type == 0xA7) {  // put
        // Receive the file from the client

        // safe recv needed
        char filename[ntohl(commandHeader.m_length) - 12];
        recv(clientSocket, filename, sizeof(filename), 0);

        putFile(clientSocket, filename);
        std::cout << "putFile operated for client" << std::endl;
        return false;
    }
    if (commandHeader.m_type == 0xA9) {  // sha256
        // Extract the filename from the SHA_REQUEST payload
        char filenameBuffer[ntohl(commandHeader.m_length) - 12];
        recv(clientSocket, filenameBuffer, sizeof(filenameBuffer), 0);
        std::string filename(filenameBuffer);

        // Calculate the SHA-256 checksum and send the response
        sha256File(clientSocket, filename);
        std::cout << "sha256File operated for client" << std::endl;
        return false;
    }

    if (commandHeader.m_type == 0xAB) {  // quit
        // The client requested to close the connection
        Header quitReply(0xAC, 0, 12);
        send(clientSocket, &quitReply, sizeof(quitReply), 0);

        // DO YOU REALLY CLOSE THE SOCKET?
        close(clientSocket);
        std::cout << "Quit from client" << std::endl;
        return true;
    }
    std::cerr << "Unexpected request type from client. Failed to provide service." << std::endl;
    return false;
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <IP address> <port>" << std::endl;
        exit(EXIT_FAILURE);
    }

    const char* ip = argv[1];
    int port = std::stoi(argv[2]);
    std::cout << "Server listening " << ip << ":" << port << std::endl;
    int serverSocket = createServerSocket(ip, port);

    while (true) {
        int clientSocket = acceptClientConnection(serverSocket);

        if (clientSocket != -1) {
            // Handle client commands in a separate thread or process
            while (true) {
                if (handleCommands(clientSocket)) { break; }
            }
        }
    }

    close(serverSocket);

    return 0;
}