/*
 * AI-Based Congestion Control for Wireless Networks
 * ns-3.46.1 | C++ | Python | Random Forest
 *
 * Optimized Implementation:
 * Uses a persistent Python worker process (predict_worker.py) communicating
 * via local POSIX stdin/stdout pipes. This eliminates the massive overhead of
 * repeatedly launching the Python interpreter, importing libraries (scikit-learn,
 * pandas, joblib), and deserializing the Random Forest model for every single
 * prediction.
 *
 * Targeted Reliability Features:
 * - Restores original SIGPIPE disposition during cleanup; child restores SIG_DFL before execvp().
 * - SafeCloseFd(): Invalidate descriptor before close(), no blind retry on EINTR (Linux semantics).
 * - Improved fgets() loop: Resets errno, checks ferror() && errno == EINTR, calls clearerr() before retry, handles EOF separately.
 * - Robust Stop(): Handles EINTR and ECHILD in waitpid explicitly, does not silently discard child PID on unexpected waitpid error.
 * - Idempotent resource cleanup supporting partial initialization and multiple calls.
 * - Clear distinction in summary between instrumented measurements and architectural guarantees.
 */

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/wifi-module.h"
#include "ns3/mobility-module.h"
#include "ns3/applications-module.h"

#include <iostream>
#include <fstream>
#include <iomanip>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <vector>
#include <string>
#include <chrono>
#include <cerrno>
#include <cstring>

// POSIX headers for persistent process communication
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("AIAdaptiveCongestionControl");

// Structure to track per-sender simulation state
struct SenderState
{
    Ptr<PacketSink> sink;
    Ptr<OnOffApplication> app;
    uint64_t lastRx;
    double rateMbps;
    uint32_t id;
};

std::vector<SenderState> senders;
std::ofstream results;

/**
 * Class: PersistentPythonWorkerClient
 *
 * Manages the lifecycle of a dedicated, long-running Python worker process.
 * Replaces repeated popen() calls with a single persistent process connected
 * via bidirectional POSIX standard I/O pipes.
 */
class PersistentPythonWorkerClient
{
public:
    PersistentPythonWorkerClient()
        : m_pid(-1),
          m_writeFd(-1),
          m_readFd(-1),
          m_writeStream(nullptr),
          m_readStream(nullptr),
          m_isReady(false),
          m_sigpipeSaved(false),
          m_launchCount(0),
          m_predictionCount(0),
          m_successCount(0),
          m_startupTimeMs(0.0)
    {
        std::memset(&m_oldSigpipeAction, 0, sizeof(m_oldSigpipeAction));
    }

    ~PersistentPythonWorkerClient()
    {
        Stop();
    }

    /**
     * Start the persistent worker process.
     * Uses fork() and execvp() with direct argument passing to avoid shell injection.
     */
    bool Start(const std::string& pythonPath = ".venv/bin/python",
               const std::string& scriptPath = "predict_worker.py")
    {
        auto startTime = std::chrono::steady_clock::now();

        // 1. Save original SIGPIPE disposition and ignore SIGPIPE in parent
        struct sigaction sa;
        std::memset(&sa, 0, sizeof(sa));
        sa.sa_handler = SIG_IGN;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        if (sigaction(SIGPIPE, &sa, &m_oldSigpipeAction) != 0)
        {
            std::cerr << "[PersistentPythonWorker] ERROR: Failed to configure SIGPIPE handling ("
                      << strerror(errno) << "). Aborting worker initialization.\n";
            return false;
        }
        m_sigpipeSaved = true;

        int pipeParentToChild[2] = {-1, -1}; // Parent writes to [1], Child reads from [0]
        int pipeChildToParent[2] = {-1, -1}; // Child writes to [1], Parent reads from [0]

        if (pipe(pipeParentToChild) < 0)
        {
            std::cerr << "[PersistentPythonWorker] ERROR: pipeParentToChild creation failed ("
                      << strerror(errno) << ").\n";
            Stop();
            return false;
        }

        if (pipe(pipeChildToParent) < 0)
        {
            std::cerr << "[PersistentPythonWorker] ERROR: pipeChildToParent creation failed ("
                      << strerror(errno) << ").\n";
            SafeCloseFd(pipeParentToChild[0]);
            SafeCloseFd(pipeParentToChild[1]);
            Stop();
            return false;
        }

        m_pid = fork();
        if (m_pid < 0)
        {
            std::cerr << "[PersistentPythonWorker] ERROR: fork() failed ("
                      << strerror(errno) << ").\n";
            SafeCloseFd(pipeParentToChild[0]);
            SafeCloseFd(pipeParentToChild[1]);
            SafeCloseFd(pipeChildToParent[0]);
            SafeCloseFd(pipeChildToParent[1]);
            Stop();
            return false;
        }

        if (m_pid == 0)
        {
            // === Child Process (Python Worker) ===
            // Restore default SIGPIPE handling in the child process before execvp
            struct sigaction sa_child;
            std::memset(&sa_child, 0, sizeof(sa_child));
            sa_child.sa_handler = SIG_DFL;
            sigemptyset(&sa_child.sa_mask);
            sa_child.sa_flags = 0;
            sigaction(SIGPIPE, &sa_child, nullptr);

            // Direct child stdin from pipeParentToChild[0]
            dup2(pipeParentToChild[0], STDIN_FILENO);
            // Direct child stdout to pipeChildToParent[1]
            dup2(pipeChildToParent[1], STDOUT_FILENO);

            // Close duplicate file descriptors in child
            close(pipeParentToChild[0]);
            close(pipeParentToChild[1]);
            close(pipeChildToParent[0]);
            close(pipeChildToParent[1]);

            // Arguments array (avoiding shell execution entirely)
            char* const argv[] = {
                const_cast<char*>(pythonPath.c_str()),
                const_cast<char*>(scriptPath.c_str()),
                nullptr
            };

            execvp(argv[0], argv);

            // Reached only if execvp fails
            std::cerr << "[Child Worker] ERROR: Failed to execute '" << pythonPath << " " << scriptPath
                      << "' (" << strerror(errno) << ").\n";
            _exit(127);
        }

        // === Parent Process (ns-3 Simulation) ===
        // Close pipe ends unused by parent
        SafeCloseFd(pipeParentToChild[0]);
        SafeCloseFd(pipeChildToParent[1]);

        m_writeFd = pipeParentToChild[1];
        m_readFd = pipeChildToParent[0];

        m_writeStream = fdopen(m_writeFd, "w");
        m_readStream = fdopen(m_readFd, "r");

        if (!m_writeStream || !m_readStream)
        {
            std::cerr << "[PersistentPythonWorker] ERROR: fdopen failed ("
                      << strerror(errno) << ").\n";
            Stop();
            return false;
        }

        // Wait for READY handshake from Python worker
        char handshakeBuf[256];
        std::string readError;
        if (ReadLine(handshakeBuf, sizeof(handshakeBuf), readError))
        {
            std::string line(handshakeBuf);
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' '))
            {
                line.pop_back();
            }

            if (line == "READY")
            {
                auto endTime = std::chrono::steady_clock::now();
                m_startupTimeMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();
                m_isReady = true;
                m_launchCount = 1;
                std::cout << "[PersistentPythonWorker] Worker initialized and model loaded in "
                          << std::fixed << std::setprecision(2) << m_startupTimeMs << " ms.\n";
                return true;
            }
            else
            {
                std::cerr << "[PersistentPythonWorker] ERROR: Unexpected worker handshake: '" << line << "'\n";
            }
        }
        else
        {
            std::cerr << "[PersistentPythonWorker] ERROR: Failed to read handshake from worker ("
                      << readError << ").\n";
        }

        Stop();
        return false;
    }

    /**
     * Request a congestion prediction for the given rate and throughput.
     * Returns: 0 (Normal), 1 (Congested), or -1 (Error).
     */
    int Predict(double rateMbps, double throughputMbps)
    {
        if (!m_isReady || !m_writeStream || !m_readStream)
        {
            std::cerr << "[PersistentPythonWorker] ERROR: Worker is not running or ready!\n";
            return -1;
        }

        // Send feature values matching exact training order: Rate_Mbps, Throughput_Mbps
        int writeBytes = fprintf(m_writeStream, "%.4f %.4f\n", rateMbps, throughputMbps);
        if (writeBytes < 0)
        {
            std::cerr << "[PersistentPythonWorker] ERROR: Failed to write to worker pipe (errno="
                      << errno << ": " << strerror(errno) << ").\n";
            m_isReady = false;
            return -1;
        }

        if (fflush(m_writeStream) != 0)
        {
            std::cerr << "[PersistentPythonWorker] ERROR: Failed to flush worker pipe (errno="
                      << errno << ": " << strerror(errno) << ").\n";
            m_isReady = false;
            return -1;
        }

        // Read prediction response line, safely handling EINTR and stream state
        char respBuf[128];
        std::string readError;
        if (!ReadLine(respBuf, sizeof(respBuf), readError))
        {
            std::cerr << "[PersistentPythonWorker] ERROR: Failed to read prediction from worker ("
                      << readError << ").\n";
            m_isReady = false;
            return -1;
        }

        m_predictionCount++;

        try
        {
            int prediction = std::stoi(respBuf);
            if (prediction == 0 || prediction == 1)
            {
                m_successCount++;
                return prediction;
            }
            else
            {
                std::cerr << "[PersistentPythonWorker] ERROR: Received non-binary prediction: " << prediction << "\n";
                return -1;
            }
        }
        catch (const std::exception& e)
        {
            std::cerr << "[PersistentPythonWorker] ERROR: Parsing error for response '" << respBuf << "': " << e.what() << "\n";
            return -1;
        }
    }

    /**
     * Stop the worker gracefully and clean up all process and file descriptor resources.
     * Guaranteed to be idempotent and safe when called multiple times or with partially
     * initialized resources.
     */
    void Stop()
    {
        if (m_isReady && m_writeStream)
        {
            // Attempt clean quit notification; ignore broken pipe errors
            if (fprintf(m_writeStream, "QUIT\n") >= 0)
            {
                fflush(m_writeStream);
            }
        }

        m_isReady = false;

        // Safely close write stream and descriptor
        if (m_writeStream)
        {
            fclose(m_writeStream);
            m_writeStream = nullptr;
            m_writeFd = -1; // fclose closes the underlying descriptor
        }
        else if (m_writeFd >= 0)
        {
            SafeCloseFd(m_writeFd);
        }

        // Safely close read stream and descriptor
        if (m_readStream)
        {
            fclose(m_readStream);
            m_readStream = nullptr;
            m_readFd = -1; // fclose closes the underlying descriptor
        }
        else if (m_readFd >= 0)
        {
            SafeCloseFd(m_readFd);
        }

        // Reap child process with explicit EINTR and ECHILD handling
        if (m_pid > 0)
        {
            int status = 0;
            int pollRes = PollProcessStatus(m_pid, status);
            if (pollRes == 1)
            {
                m_pid = -1;
            }
            else if (pollRes == 0)
            {
                // Give worker a brief moment (100 ms) to exit cleanly
                usleep(100000);
                pollRes = PollProcessStatus(m_pid, status);
                if (pollRes == 1)
                {
                    m_pid = -1;
                }
                else if (pollRes == 0)
                {
                    kill(m_pid, SIGTERM);
                    usleep(100000);
                    pollRes = PollProcessStatus(m_pid, status);
                    if (pollRes == 1)
                    {
                        m_pid = -1;
                    }
                    else if (pollRes == 0)
                    {
                        kill(m_pid, SIGKILL);
                        int waitRes = WaitForProcessExit(m_pid, status);
                        if (waitRes == 1)
                        {
                            m_pid = -1;
                        }
                        // If waitRes == -1, an unexpected waitpid error occurred.
                        // It was logged inside WaitForProcessExit; we do not silently clear m_pid.
                    }
                }
            }
        }

        // Restore original SIGPIPE action in parent
        if (m_sigpipeSaved)
        {
            sigaction(SIGPIPE, &m_oldSigpipeAction, nullptr);
            m_sigpipeSaved = false;
        }
    }

    uint32_t GetLaunchCount() const { return m_launchCount; }
    uint32_t GetPredictionCount() const { return m_predictionCount; }
    uint32_t GetSuccessCount() const { return m_successCount; }
    double GetStartupTimeMs() const { return m_startupTimeMs; }
    bool IsReady() const { return m_isReady; }

private:
    /**
     * SafeCloseFd: Close descriptor once, invalidate stored fd immediately,
     * and do not retry on EINTR (on Linux/WSL, close() always releases the descriptor).
     */
    static void SafeCloseFd(int& fd)
    {
        if (fd >= 0)
        {
            int tempFd = fd;
            fd = -1; // Invalidate first to prevent double-closing or racing
            if (close(tempFd) < 0)
            {
                if (errno != EINTR)
                {
                    std::cerr << "[PersistentPythonWorker] WARNING: close() failed on fd "
                              << tempFd << " (" << strerror(errno) << ").\n";
                }
            }
        }
    }

    /**
     * ReadLine: Reads a line from stream with proper errno reset,
     * retrying only if ferror() is set and errno == EINTR. Handles EOF separately.
     */
    bool ReadLine(char* buf, size_t size, std::string& errorDetail)
    {
        if (!m_readStream)
        {
            errorDetail = "Stream is null";
            return false;
        }

        while (true)
        {
            errno = 0;
            char* ptr = fgets(buf, size, m_readStream);
            if (ptr != nullptr)
            {
                return true; // Successfully read line
            }

            if (ferror(m_readStream))
            {
                if (errno == EINTR)
                {
                    clearerr(m_readStream);
                    continue; // Interrupted by signal; clear error and retry
                }
                errorDetail = std::string("Stream error: ") + strerror(errno);
                return false;
            }

            if (feof(m_readStream))
            {
                errorDetail = "End of file (worker closed output pipe)";
                return false;
            }

            errorDetail = "Unknown read failure";
            return false;
        }
    }

    /**
     * Non-blocking poll for child process termination with EINTR and ECHILD handling.
     * Returns: 1 if child exited / reaped, 0 if still running, -1 on unexpected error.
     */
    static int PollProcessStatus(pid_t pid, int& status)
    {
        while (true)
        {
            errno = 0;
            pid_t res = waitpid(pid, &status, WNOHANG);
            if (res > 0)
            {
                return 1; // Child reaped
            }
            if (res == 0)
            {
                return 0; // Child still running
            }
            if (errno == EINTR)
            {
                continue; // Interrupted by signal, retry
            }
            if (errno == ECHILD)
            {
                return 1; // Process does not exist or already reaped
            }
            std::cerr << "[PersistentPythonWorker] ERROR: waitpid(WNOHANG) failed for PID "
                      << pid << " (" << strerror(errno) << ").\n";
            return -1;
        }
    }

    /**
     * Blocking wait for child process termination with EINTR and ECHILD handling.
     * Returns: 1 if child reaped / gone, -1 on unexpected error.
     */
    static int WaitForProcessExit(pid_t pid, int& status)
    {
        while (true)
        {
            errno = 0;
            pid_t res = waitpid(pid, &status, 0);
            if (res > 0)
            {
                return 1; // Child reaped
            }
            if (errno == EINTR)
            {
                continue; // Interrupted by signal, retry
            }
            if (errno == ECHILD)
            {
                return 1; // Already reaped or gone
            }
            std::cerr << "[PersistentPythonWorker] ERROR: blocking waitpid failed for PID "
                      << pid << " (" << strerror(errno) << ").\n";
            return -1;
        }
    }

    pid_t m_pid;
    int m_writeFd;
    int m_readFd;
    FILE* m_writeStream;
    FILE* m_readStream;
    bool m_isReady;

    struct sigaction m_oldSigpipeAction;
    bool m_sigpipeSaved;

    uint32_t m_launchCount;
    uint32_t m_predictionCount;
    uint32_t m_successCount;
    double m_startupTimeMs;
};

// Global instance of the persistent worker client
static PersistentPythonWorkerClient g_workerClient;

// Ask the trained Python model for a prediction via the persistent worker
int PredictCongestion(double rate, double throughput)
{
    return g_workerClient.Predict(rate, throughput);
}

// Run every second to measure and control network traffic
void ControlLoop()
{
    std::cout << "\n========== AI CONTROL: "
              << Simulator::Now().GetSeconds()
              << " seconds ==========\n";

    for (auto& sender : senders)
    {
        uint64_t currentRx = sender.sink->GetTotalRx();

        uint64_t receivedBytes = currentRx - sender.lastRx;

        // Throughput measured during the last 1-second window
        double throughput = receivedBytes * 8.0 / 1000000.0;

        sender.lastRx = currentRx;

        int prediction = PredictCongestion(sender.rateMbps, throughput);

        if (prediction == -1)
        {
            std::cerr << "Prediction failed for sender "
                      << sender.id
                      << ". Keeping its current rate.\n";

            results << Simulator::Now().GetSeconds() << ","
                    << sender.id << ","
                    << sender.rateMbps << ","
                    << throughput << ",ERROR\n";

            continue;
        }

        std::string status = prediction == 1 ? "CONGESTED" : "NORMAL";

        double oldRate = sender.rateMbps;

        // Congestion: multiplicative decrease (30% cut, minimum 1.0 Mbps)
        if (prediction == 1)
        {
            sender.rateMbps = std::max(1.0, sender.rateMbps * 0.70);
        }
        // Normal: cautious additive increase (+1.0 Mbps, maximum 30.0 Mbps)
        else
        {
            sender.rateMbps = std::min(30.0, sender.rateMbps + 1.0);
        }

        // Update the sender's actual transmission rate
        uint64_t bitsPerSecond = static_cast<uint64_t>(sender.rateMbps * 1000000.0);

        sender.app->SetAttribute(
            "DataRate",
            DataRateValue(DataRate(bitsPerSecond))
        );

        std::cout << "Sender " << sender.id
                  << " | Throughput: " << throughput
                  << " Mbps"
                  << " | AI: " << status
                  << " | Rate: " << oldRate
                  << " -> " << sender.rateMbps
                  << " Mbps\n";

        results << Simulator::Now().GetSeconds() << ","
                << sender.id << ","
                << sender.rateMbps << ","
                << throughput << ","
                << status << "\n";
    }

    // Continue controlling until the experiment ends
    if (Simulator::Now() < Seconds(10.0))
    {
        Simulator::Schedule(
            Seconds(1.0),
            &ControlLoop
        );
    }
}

int main(int argc, char* argv[])
{
    auto simWallClockStart = std::chrono::steady_clock::now();

    results.open("ai-control-results.csv");

    if (!results.is_open())
    {
        std::cerr << "Cannot create results CSV!\n";
        return 1;
    }

    results << "Time,Sender_ID,Rate_Mbps,"
            << "Throughput_Mbps,AI_Prediction\n";

    // Initialize the persistent Python prediction worker once before simulation run
    std::cout << "\n[Setup] Initializing persistent Python prediction worker...\n";
    if (!g_workerClient.Start(".venv/bin/python", "predict_worker.py"))
    {
        std::cerr << "[Setup] FATAL: Failed to initialize persistent Python worker!\n";
        results.close();
        return 1;
    }

    RngSeedManager::SetSeed(12345);
    RngSeedManager::SetRun(1);

    // Create 3 senders and 1 receiver
    NodeContainer nodes;
    nodes.Create(4);

    // Configure Wi-Fi
    WifiHelper wifi;
    wifi.SetStandard(WIFI_STANDARD_80211n);

    YansWifiChannelHelper channel = YansWifiChannelHelper::Default();

    YansWifiPhyHelper phy;
    phy.SetChannel(channel.Create());

    WifiMacHelper mac;
    mac.SetType("ns3::AdhocWifiMac");

    NetDeviceContainer devices = wifi.Install(phy, mac, nodes);

    // Set node positions
    MobilityHelper mobility;

    Ptr<ListPositionAllocator> positions = CreateObject<ListPositionAllocator>();

    positions->Add(Vector(0.0, 0.0, 0.0));
    positions->Add(Vector(5.0, 0.0, 0.0));
    positions->Add(Vector(0.0, 5.0, 0.0));
    positions->Add(Vector(5.0, 5.0, 0.0));

    mobility.SetPositionAllocator(positions);

    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");

    mobility.Install(nodes);

    // Install Internet stack
    InternetStackHelper internet;
    internet.Install(nodes);

    // Assign IP addresses
    Ipv4AddressHelper address;

    address.SetBase(
        "10.1.1.0",
        "255.255.255.0"
    );

    Ipv4InterfaceContainer interfaces = address.Assign(devices);

    // Install one separate receiver port per sender
    for (uint32_t i = 0; i < 3; i++)
    {
        uint16_t port = 5000 + i;

        PacketSinkHelper sinkHelper(
            "ns3::UdpSocketFactory",
            InetSocketAddress(
                Ipv4Address::GetAny(),
                port
            )
        );

        ApplicationContainer sinkApps = sinkHelper.Install(nodes.Get(3));

        sinkApps.Start(Seconds(0.0));
        sinkApps.Stop(Seconds(12.0));

        Ptr<PacketSink> sink = DynamicCast<PacketSink>(sinkApps.Get(0));

        // Configure sender
        OnOffHelper source(
            "ns3::UdpSocketFactory",
            InetSocketAddress(
                interfaces.GetAddress(3),
                port
            )
        );

        source.SetAttribute("PacketSize", UintegerValue(1024));

        source.SetAttribute(
            "OnTime",
            StringValue("ns3::ConstantRandomVariable[Constant=1]")
        );

        source.SetAttribute(
            "OffTime",
            StringValue("ns3::ConstantRandomVariable[Constant=0]")
        );

        double initialRate = 15.0;

        source.SetAttribute(
            "DataRate",
            DataRateValue(DataRate(15000000))
        );

        ApplicationContainer sourceApps = source.Install(nodes.Get(i));

        sourceApps.Start(Seconds(1.0 + i * 0.2));
        sourceApps.Stop(Seconds(11.0 + i * 0.2));

        Ptr<OnOffApplication> app = DynamicCast<OnOffApplication>(sourceApps.Get(0));

        SenderState state;
        state.sink = sink;
        state.app = app;
        state.lastRx = 0;
        state.rateMbps = initialRate;
        state.id = i + 1;

        senders.push_back(state);
    }

    // Start feedback control after traffic begins
    Simulator::Schedule(
        Seconds(2.0),
        &ControlLoop
    );

    Simulator::Stop(Seconds(14.0));

    std::cout << "\nStarting AI-based adaptive simulation...\n";

    Simulator::Run();

    Simulator::Destroy();

    results.close();

    // Cleanly terminate persistent Python worker
    g_workerClient.Stop();

    auto simWallClockEnd = std::chrono::steady_clock::now();
    double totalWallClockSec = std::chrono::duration<double>(simWallClockEnd - simWallClockStart).count();

    std::cout << "\n=======================================================\n";
    std::cout << "AI CONTROL SIMULATION COMPLETED!\n";
    std::cout << "Results: ai-control-results.csv\n";
    std::cout << "-------------------------------------------------------\n";
    std::cout << "OPTIMIZATION BENCHMARK SUMMARY:\n";
    std::cout << "  Simulated Duration            : 14.00 s (ns-3 simulated time)\n";
    std::cout << "  Total Wall-Clock Time         : " << std::fixed << std::setprecision(2) << totalWallClockSec << " s [Instrumented Measurement]\n";
    std::cout << "  Worker Startup Duration       : " << std::fixed << std::setprecision(2) << g_workerClient.GetStartupTimeMs() << " ms [Instrumented Measurement]\n";
    std::cout << "  Python Worker Launches        : " << g_workerClient.GetLaunchCount() << " [Instrumented Counter; Architectural Guarantee: 1]\n";
    std::cout << "  Model Loads in Memory         : 1 [Architecture-Derived: resident in memory throughout worker lifecycle]\n";
    std::cout << "  Predictions Requested         : " << g_workerClient.GetPredictionCount() << " [Instrumented Counter; Expected: 27]\n";
    std::cout << "  Predictions Succeeded         : " << g_workerClient.GetSuccessCount() << " [Instrumented Counter; Expected: 27]\n";
    std::cout << "  Baseline Comparison Reference : 27 Launches / 27 Model Loads [Historical 1-per-inference popen baseline]\n";
    std::cout << "=======================================================\n";

    return 0;
}
