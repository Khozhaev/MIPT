#include <iostream>
#include <string>
#include <vector>
#include <optional>
#include <algorithm>
#include <sstream>
#include <strstream>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <memory>
#include <fstream>
#include <dirent.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/timeb.h>
#include <iomanip>

//#define DEBUG

typedef struct timeval timeval;

timeval& operator-=(timeval& lhs, const timeval& rhs){
    if(lhs.tv_usec < rhs.tv_usec){
        --lhs.tv_sec;
        lhs.tv_usec = 1000 - rhs.tv_usec + lhs.tv_usec;
    }
    lhs.tv_sec -= rhs.tv_sec;
    return lhs;
}

class Timer{
    bool isLog_;
    int who_;
    rusage start_;
    timeval timerStart_;
    struct timezone zone_;
public:
    Timer(bool isLog, int type = RUSAGE_CHILDREN)
            : isLog_(isLog),
              who_(type),
              zone_({0, 0})
    {
        gettimeofday(&timerStart_, &zone_);
        getrusage(type, &start_);
    }
    ~Timer(){
        if(!isLog_) {
            return;
        }
        {
            timeval timerEnd_;
            gettimeofday(&timerEnd_, &zone_);
            timerEnd_ -= timerStart_;
            std::cerr << "real         " << (timerEnd_.tv_sec) / 60 << "m" << (timerEnd_.tv_sec)%60 << "," << std::setw(3)
                      << std::setfill('0') << std::left << timerEnd_.tv_usec / 1000 << "s" << std::endl;
        }
        rusage end_;
        getrusage(who_, &end_);
        end_.ru_utime -= start_.ru_utime;
        std::cerr << "user         " << (end_.ru_utime.tv_sec) / 60 << "m" << (end_.ru_utime.tv_sec)%60 << "," << std::setw(3)
                  << std::setfill('0') << std::left << end_.ru_utime.tv_usec / 1000 << "s" << std::endl;
        end_.ru_stime -= start_.ru_stime;
        std::cerr << "sys          " << (end_.ru_stime.tv_sec) / 60 << "m" << (end_.ru_stime.tv_sec)%60 << "," << std::setw(3)
                  << std::setfill('0') << std::left << end_.ru_stime.tv_usec / 1000 << "s" << std::endl;
    }
};
void sig_handler(int sig) {
    signal(SIGINT, sig_handler);
};

class Matcher{
    std::string pattern_;
    bool Matching(
            std::string::const_iterator patternBegin,
            std::string::const_iterator patternEnd,
            std::string::const_iterator candidateBegin,
            std::string::const_iterator candidateEnd) const
    {
        if(patternBegin == patternEnd) {
            return candidateBegin == candidateEnd;
        }
        if(*patternBegin == '*'){
            bool res = false;
            for (auto it = candidateBegin; it != candidateEnd; ++it) {
                res |= Matching(patternBegin + 1, patternEnd, it, candidateEnd);
            }
            res |= Matching(patternBegin + 1, patternEnd, candidateEnd, candidateEnd);
            return res;
        }
        if(candidateBegin == candidateEnd){
            return false;
        }
        if(*patternBegin == '?'){
            return Matching(patternBegin + 1, patternEnd, candidateBegin, candidateEnd);
        }
        return (*patternBegin == *candidateBegin)
               && (Matching(patternBegin + 1,patternEnd, candidateBegin + 1, candidateEnd));
    }
public:
    Matcher(std::string pattern)
            :pattern_(std::move(pattern))
    {}
    bool operator()(const std::string& candidate) const{
        return Matching(pattern_.begin(), pattern_.end(), candidate.begin(), candidate.end());
    }
};

template<typename T>
std::istream& operator>>(std::istream& in, std::optional<T>& x){
    T t;
    in >> t;
    x = t;
    return in;
}

template<typename F>
std::string helpSysGetter(F f){
    size_t buffSz = 20;
    char *buff = new char[buffSz];

    while(!f(buff, buffSz)){
        buffSz *= 2;
        delete[] buff;
        buff = new char[buffSz];
    }
    std::string res(buff);
    delete[] buff;
    return res;
}

std::string getCurrDir(){
    return helpSysGetter([](char* buff, size_t buffSz){return getcwd(buff, buffSz);});
}

std::string getCurrUserName(){
    return helpSysGetter([](char *buff, size_t buffSz){
                             return !getlogin_r(buff, buffSz);
                         }
    );
}




class Command{
    std::string commandName_;
    std::vector<char *> args_;
    std::optional<int> inputFD_, outputFD_;
    std::vector<int> closerFD_;
    std::optional<std::string> inputFile_, outputFile_;

    static void CD_(const std::string& newDir){
        if(chdir(newDir.c_str()))
            throw std::invalid_argument("change dir error");
    }
    static void PWD_(int fd) {
        std::string curDir = getCurrDir();
        write(fd, curDir.c_str(), curDir.size());
    }
    void CD_() {
        std::string newDir;
        if(args_.size() < 2 || args_[1] == nullptr || !args_[1][0]){
            newDir = "/home/" + getCurrUserName();
        } else if(args_[1][0] == '/'){
            newDir = args_[1][0];
        } else if(args_[1][0] == '~'){
            newDir = "/home/" + getCurrUserName();
            if(args_[1][1]){
                newDir += (args_[1] + 1);
            }
        } else {
            newDir = getCurrDir() += '/';
            newDir += (args_[1]);
        }
        CD_(newDir);
    }
    void PWD_() {
        if(!fork()){
            if(outputFD_){
                PWD_(*outputFD_);
            } else if(outputFile_){
                PWD_(open(outputFile_->c_str(), O_WRONLY | O_CREAT, 0777));
            } else{
                PWD_(1);
            }
            exit(0);
        } else{
            return;
        }
    }
    void Time_(){
        args_.erase(args_.begin());
        commandName_ = args_[0];
        Exec();
    }
    void ExecInner_(){
        pid_t pid = fork();
        if(pid < 0){
            throw std::runtime_error("error creating the process");
        } else if(pid == 0){
            if(commandName_.empty()) return;
            args_.push_back(nullptr);
            Close_();
            if(inputFD_){
                dup2(0, *inputFD_);
            } else if(inputFile_) {
                close(0);
                open(inputFile_->c_str(), O_RDONLY | O_CREAT, 0777);
            }
            if(outputFD_){
                dup2(1, *outputFD_);
            } else if(outputFile_){
                close(1);
                open(outputFile_->c_str(), O_WRONLY | O_CREAT, 0777);
            }
            execvp(commandName_.c_str(), args_.data());
        }
    }
private:
    void Close_(){
        for(int fd : closerFD_){
            close(fd);
        }
    }
public:
    //rule of five
    Command(const Command&) = delete;
    Command(Command&&) = delete;
    Command& operator=(const Command&) = delete;
    Command& operator=(Command&&) = delete;

    Command(std::string name):
            commandName_(std::move(name)), args_(1, new char[name.size() + 1]){
        memcpy(args_[0], name.data(), name.size());
        args_[0][name.size()] = 0;
    }


    Command& AddArg(const std::string& arg){
        args_.push_back(new char [arg.size() + 1]);
        memcpy(args_.back(), arg.data(), arg.size());
        args_.back()[arg.size()] = 0;
        return *this;
    }

    Command& AddArg(const std::vector<std::string>& args){
        for(const auto& arg : args){
            AddArg(arg);
        }
        return *this;
    }
    Command& AddCloserFD(int fd){
        closerFD_.push_back(fd);
        return *this;
    }
    Command& SetInputFd(int fd){
        inputFD_ = fd;
        return *this;
    }
    Command& SetOutputFD(int fd){
        outputFD_ = fd;
        return *this;
    }
    Command& SetInputFile(std::string input){
        inputFile_ = input;
        return *this;
    }
    Command& SetOutputFile(std::string output){
        outputFile_ = output;
        return *this;
    }

    void Exec() {
        if(commandName_ == "cd"){
            CD_();
        } else if(commandName_ == "pwd") {
            PWD_();
        } else if(commandName_ == "time") {
            Time_();
        } else{
            ExecInner_();
        }
    }

    ~Command(){
        for(char * arg : args_){
            delete[] arg;
        }
    }

};

struct CommandLine{
    std::optional<std::string> inputFile;
    std::optional<std::string> outputFile;
    std::vector<std::pair<std::string, std::vector<std::string>>> commands;
    bool isLogTime = false;
};


void FindMatchesDFS(const std::string &curDir, std::string::const_iterator beginPattern, std::string::const_iterator endPattern, std::vector<std::string>& res) {
    DIR* dir = opendir(curDir.c_str());
    if(beginPattern == endPattern) return;
    if(!dir) return;
    auto nextStart = std::find(beginPattern, endPattern, '/');
    Matcher matcher(std::string(beginPattern, nextStart));
    for(dirent *d = readdir(dir); d != nullptr; d = readdir(dir)) {
        std::string name = d->d_name;
        if(matcher(name)){
            std::string fullName = curDir;
            if(curDir != "/")
                fullName += '/';
            fullName += name;
            if(nextStart == endPattern){
                res.push_back(fullName);
                continue;
            } else if(d->d_type == DT_DIR){
                FindMatchesDFS(fullName, nextStart + 1, endPattern, res);
            } else{
                continue;
            }
        }
    }
}

std::vector<std::string> getMatches(const std::string& pattern){
    if(pattern.empty()) {
        return {};
    }
    std::vector<std::string> res;
    if(pattern[0] == '/'){
        FindMatchesDFS("/", pattern.begin() + 1, pattern.end(), res);
    } else if(pattern[0] == '~') {
        FindMatchesDFS("~", pattern.begin() + 1, pattern.end(), res);
    } else{
        FindMatchesDFS(getCurrDir(), pattern.begin(), pattern.end(), res);
    }
    return res;
}

void parseArgsTo(const std::string& arg, std::vector<std::string>& args) {
    if(std::find(arg.begin(), arg.end(), '*') == arg.end()) {
        args.push_back(arg);
        return;
    }
    auto files = getMatches(arg);
    if(files.empty()){
        throw std::invalid_argument("invalid mask (don't found files with this mask): " + arg);
    }
    for(const auto& file : files) {
        args.push_back(file);
    }
}


class Parser{
private:
    std::istream& in_;
    static CommandLine ParseLine(const std::string& line){
        CommandLine res;
        auto startCommand = line.begin();
        while(startCommand != line.end()){
            auto endCommand = std::find(next(startCommand), line.end(), '|');
            auto fileInput = std::find(startCommand, endCommand, '<');
            if(fileInput != endCommand && std::find(next(fileInput), endCommand, '<') != endCommand){
                throw std::invalid_argument("command can't have more 1 input file");
            }
            auto fileOutput = std::find(startCommand, endCommand, '>');
            if(fileOutput != endCommand && std::find(next(fileOutput), endCommand, '>') != endCommand){
                throw std::invalid_argument("command can't have more 1 output file");
            }
            if(fileInput != endCommand){
                if(startCommand != line.begin()) {
                    throw std::invalid_argument("The symbol '<' is not allowed in the pipeline component.");
                }
                std::istringstream fileNameParse(std::string(next(fileInput), endCommand));
                if(res.inputFile){
                    throw std::invalid_argument("command can't have more 1 input file");
                }
                fileNameParse >> res.inputFile;
            }
            if(fileOutput != endCommand){
                if(endCommand != line.end()){
                    throw std::invalid_argument("The symbol '>' is not allowed in the pipeline component.");
                }
                std::istringstream fileNameParse(std::string(next(fileOutput), endCommand));
                if(res.outputFile){
                    throw std::invalid_argument("command can't have more 1 output file");
                }
                fileNameParse >> res.outputFile;
            }
            std::istringstream commandParse(std::string((startCommand), endCommand));
            std::string commandName;
            std::vector<std::string> commandArgs;
            commandParse >> commandName;
            if(commandName == "time") {
                res.isLogTime = true;
            }
            std::string arg;
            while(commandParse >> arg){
                if(arg[0] == '>' || arg[0] == '<'){
                    if(arg.size() == 1){
                        commandParse >> arg;
                    }
                    continue;
                }
                parseArgsTo(arg, commandArgs);
            }
            res.commands.emplace_back(std::move(commandName), std::move(commandArgs));
            startCommand = endCommand == line.end() ? endCommand : next(endCommand);
        }
        return res;
    }

public:
    Parser(std::istream& in): in_(in) {}

    CommandLine GetLine(){
        std::string line;
        if(!getline(in_, line)){
            throw std::string("STOP");
        }
        return ParseLine(line);
    }

};


class LineRunning{
private:
    std::vector<std::unique_ptr<Command>> PrepareLine(CommandLine line){
        if(line.commands.empty()){
            return {};
        }
        std::vector<std::unique_ptr<Command>> res(line.commands.size());
#ifdef DEBUG
        CommandLingDebugMod(line);
#endif
        for(size_t i = 0; i < res.size(); ++i){
            res[i] = std::make_unique<Command>(line.commands[i].first);
            res[i]->AddArg(line.commands[i].second);
        }
        if(line.inputFile){
            res[0]->SetInputFile(*line.inputFile);
        }
        if(line.outputFile){
            res.back()->SetOutputFile(*line.outputFile);
        }
        for(size_t i = 0; i + 1 < res.size(); ++i){
            int fd[2];
            pipe(fd);
            res[i]->SetInputFd(fd[0]);
            res[i]->AddCloserFD(fd[1]);
            res[i + 1]->SetOutputFD(fd[1]);
            res[i + 1]->AddCloserFD(fd[0]);
        }
        return res;
    }
public:
    void ExecLine(CommandLine line){
        Timer timer(line.isLogTime);
        auto commands = PrepareLine(std::move(line));
        for(auto& command : commands){
            command->Exec();
        }
        waitpid(-1, NULL, 0);
    }
};

class Worker{
private:
    Parser parser_;
    LineRunning lineRunning_;
    std::ostream& out_;
    std::istream& in_;
public:
    Worker(std::istream& in, std::ostream& out): parser_(in), out_(out), in_(in) {}
    void Run(){
        while(true){
            try{
                std::cout << getCurrDir() << (getCurrUserName() == "root" ? '!' : '>');
                auto commandLine = parser_.GetLine();
                lineRunning_.ExecLine(std::move(commandLine));
            } catch(std::exception& ex){
                std::cerr << ex.what() << std::endl;
            } catch(const std::string& ex) {
                if(ex == "STOP") {
                    return;
                } else{
                    throw;
                }
            } catch (...) {
                std::cerr << "Unknown exception";
            }
        }
    }
};

class MicroShell{
private:
    Worker worker_;
public:
    MicroShell():worker_(std::cin, std::cout){}
    void Run(){
        worker_.Run();
    }
};



int main() {
    signal(SIGINT, sig_handler);
    MicroShell().Run();
    return 0;
}
