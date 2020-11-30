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

#define DEBUG



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
    return helpSysGetter([](char *buff, size_t buffSz){return getlogin_r(buff, buffSz);});
}
void cd(const std::string& newDir = "~"){
    if(chdir(newDir.c_str()))
        throw std::invalid_argument("change dir error");
}



class Command{
    std::string commandName_;
    std::vector<char *> args_;
    std::optional<int> inputFD_, outputFD_;
    std::vector<int> closerFD_;

public:
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

    void Exec(){
        pid_t pid = fork();
        args_.push_back(nullptr);
        if(pid < 0){
            throw std::runtime_error("error creating the process");
        } else if(pid == 0){
            if(commandName_.empty()) return;
            Close_();
            if(inputFD_){
                dup2(*inputFD_, 0);
            }
            if(outputFD_){
                dup2(*outputFD_, 1);
            }
            execvp(commandName_.c_str(), args_.data());
        }
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
};

#ifdef DEBUG
void CommandLingDebugMod(const CommandLine& cmdLine){
    using namespace std;
    cerr << endl;
    if(cmdLine.inputFile){
        cerr << "Input : " << *cmdLine.inputFile << endl;
    }
    if(cmdLine.outputFile){
        cerr << "Output : " << *cmdLine.outputFile << endl;
    }
    for(const auto& p : cmdLine.commands){
        cerr << "Command Name : " << p.first << endl;
        for(const auto& arg : p.second){
            cerr << "          " << arg << endl;
        }
        cerr << "----------";
        cerr << endl;
    }

}
#endif

class Parser{
private:
    std::istream& in_;
    static CommandLine ParseLine(const std::string& line){
        CommandLine res;
        auto startCommand = line.begin();
        while(startCommand != line.end()){
            auto endCommand = std::find(next(startCommand), line.end(), '|');
            auto fileInput = std::find(startCommand, endCommand, '<');
            auto fileOutput = std::find(startCommand, endCommand, '>');
            if(fileInput != endCommand){
                if(startCommand != line.begin()) {
                    throw std::invalid_argument("The symbol '<' is not allowed in the pipeline component.");
                }
                std::istringstream fileNameParse(std::string(next(fileInput), endCommand));
                fileNameParse >> res.inputFile;
            }
            if(fileOutput != endCommand){
                if(endCommand != line.end()){
                    throw std::invalid_argument("The symbol '>' is not allowed in the pipeline component.");
                }
                std::istringstream fileNameParse(std::string(next(fileOutput), endCommand));
                fileNameParse >> res.outputFile;
            }
            std::istringstream commandParse(std::string((startCommand), endCommand));
            std::string commandName;
            std::vector<std::string> commandArgs;
            commandParse >> commandName;
            std::string arg;
            while(commandParse >> arg){
                if(arg[0] == '>' || arg[0] == '<'){
                    if(arg.size() == 1){
                        commandParse >> arg;
                    }
                    continue;
                }
                commandArgs.push_back(arg);
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
        getline(in_, line);
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
            res[0]->SetInputFd(open(line.inputFile->c_str(), O_WRONLY | O_CREAT, 0777));
        }
        if(line.outputFile){
            res.back()->SetOutputFD(open(line.outputFile->c_str(), O_RDONLY | O_CREAT, 0777));
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
                std::cout << getCurrDir() << "!";
                auto commandLine = parser_.GetLine();
                lineRunning_.ExecLine(std::move(commandLine));
            } catch(std::exception& ex){
                std::cout << ex.what();
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
    MicroShell().Run();

    return 0;
}
