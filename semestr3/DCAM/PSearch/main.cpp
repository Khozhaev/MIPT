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
#include <future>
#include <unordered_map>
#include <filesystem>
#include <fstream>
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

template<typename TChar = char, typename TSize = uint_fast16_t>
class KMP{
    class BaseKMP{
        std::vector<TSize> pref_;
        TSize sz_;

        const TSize TCharRange = (std::numeric_limits<TChar>::max() - std::numeric_limits<TChar>::min()) + 1;

        using TNode = std::vector<TSize>;

        std::vector<TNode> tree_;
    public:
        BaseKMP(const std::basic_string<TChar>& pattern)
                : pref_(pattern.size(), 0),
                  sz_(pattern.size()),
                  tree_(sz_ + 1, TNode(TCharRange, 0))
        {
            for(TSize i = 1; i < pattern.size(); ++i) {
                TSize j = pref_[i - 1];

                while(j > 0 && pattern[j] != pattern[i]) {
                    j = pref_[j - 1];
                }
                pref_[i] = j + pattern[j] == pattern[i];
            }
            {
                tree_[0][pattern[0] - std::numeric_limits<TChar>::min()] = 1;
            }
            for(TSize i = 1; i < sz_; ++i) {
                TSize j = 0;
                auto c = std::numeric_limits<TChar>::min();
                for(; c < std::numeric_limits<TChar>::max(); ++c, ++j) {
                    tree_[i][j] = (pattern[i] == c) ? i + 1 : tree_[pref_[i - 1]][j];
                }
                tree_[i][j] = (pattern[i] == c) ? i + 1 : tree_[pref_[i - 1]][j];
            }
            {
                TSize j = 0;
                auto c = std::numeric_limits<TChar>::min();
                for(; c < std::numeric_limits<TChar>::max(); ++c, ++j) {
                    tree_[sz_][j] = tree_[pref_[sz_ - 1]][j];
                }
            }
        }
        TSize getNext(TSize pos, char c) const {
            return tree_[pos][c - std::numeric_limits<TChar>::min()];
        }
    };
    std::shared_ptr<BaseKMP> kmp_;
    TSize pos_;
    TSize sz_;
public:
    //opeator=, ctor(KMP) = O(1)
    KMP(const std::basic_string<TChar>& pattern)
    : kmp_(std::make_shared<BaseKMP>(pattern)),
    pos_(0),
    sz_(pattern.size())
    {}
    void Reset() {
        pos_ = 0;

    }
    bool Iterate(char c) {
        return  sz_ == (pos_ = kmp_->getNext(pos_, c));
    }
    bool strIterate(const std::string& s) {
        bool res = false;
        for(char c : s) {
            res |= Iterate(c);
        }
        return res;
    }
};

class Searcher {
    bool isRecurse_;
    size_t threadCount_;
    std::string dir_;
    std::string pattern_;
    std::vector<std::string> files_;
    std::shared_ptr<KMP<char, uint32_t>> kmp_;

    struct FileMatch{
        std::string fileName;
        std::vector<std::pair<uint64_t, std::basic_string<char>>> lines;
    };
    template<typename Functor>
    class ThreadSearcher{
        KMP<char, uint32_t> kmp_;
        Functor functor_;
    public:
        ThreadSearcher(KMP<char, uint32_t> kmp, Functor functor)
        :kmp_(kmp),
        functor_(functor)
        {
            kmp_.Reset();
        }
        void Do(
                std::vector<std::string>::const_iterator begin,
                std::vector<std::string>::const_iterator end)
        {
            std::vector<FileMatch> result;
            for(auto it = begin; it != end; ++it) {
                kmp_.Reset();
                FileMatch res;
                std::ifstream fin(*it);
                std::string t;
                uint64_t lineNum = 0;
                while(getline(fin, t)) {
                    ++lineNum;
                    if(kmp_.strIterate(t)) {
                        res.lines.emplace_back(lineNum, t);
                    }
                }
                if(!res.lines.empty()) {
                    res.fileName = *it;
                    result.push_back(std::move(res));
                }
            }
            functor_(result);
        }

    };
    void setDir_(int argc, const char** argv) {
        for(size_t i = 1; i < argc; ++i){
            if(argv[i][0] == '/') {
                dir_ = std::string(argv[i]);
            } else if(argv[i][0] == '~') {
                dir_ = "/home/" + getCurrUserName() + std::string(argv[i] + 1);
            }
        }
    }
    void setThreadCount_(int argc, const char** argv) {
        for(size_t i = 1; i < argc; ++i) {
            if(argv[i][0] == '-' && argv[i][1] == 't'){
                threadCount_ = std::atoi(argv[i] + 2);
            }
        }
    }
    void setRecurse_(int argc, const char** argv) {
        for(size_t i = 1; i < argc; ++i) {
            if(argv[i][0] == '-' && argv[i][1] == 'n') {
                isRecurse_ = false;
            }
        }
    }
    void setPattern_(int argc, const char** argv) {
        for(size_t i = 1; i < argc; ++i) {
            if(argv[i][0] != '-' && argv[i][0] != '~' && argv[i][0] != '/') {
                pattern_ = argv[i];
            }
        }
    }
    void getFiles_() {
        if(isRecurse_) {
            for(auto& p : std::filesystem::recursive_directory_iterator(dir_)) {
                if(p.is_regular_file()) {
                    //std::cerr << p.path() << std::endl;
                    files_.push_back(p.path().string());
                }
            }
        } else {
            for(auto& p : std::filesystem::directory_iterator(dir_)) {
                if(p.is_regular_file()) {
                    files_.push_back(p.path().string());
                }
            }
        }
    }

public:
    Searcher(int argc, const char** argv)
        :isRecurse_(true),
        threadCount_(1),
        dir_(getCurrDir())
    {
        if(!argc){
            throw std::invalid_argument("need pattern");
        }
        setDir_(argc, argv);
        setPattern_(argc, argv);
        setThreadCount_(argc, argv);
        setRecurse_(argc, argv);
        kmp_ = std::make_shared<KMP<char, uint32_t>>(pattern_);
    }

    void Run(std::ostream& out) {
        getFiles_();
/*        for(const auto& p : files_) {
            std::cerr << p.string() << std::endl;
        }*/
        std::vector<std::vector<FileMatch>> result(threadCount_);
        std::vector<std::future<void>> threads;
        size_t filesByThread = (files_.size() + threadCount_ - 1)/(threadCount_);
        for(size_t i = 0; i < threadCount_; ++i) {
            size_t beg = std::min(i * filesByThread, files_.size()), end = std::min((i + 1) * filesByThread, files_.size());
            threads.push_back(std::future<void>(
                    std::async([&result, i, this, beg, end]{
                        ThreadSearcher(*(this->kmp_), [&result, i](std::vector<FileMatch> res){
                            result[i] = (std::move(res));
                        }).Do(files_.begin() + beg, files_.begin() + end);}
                    )));
        }
        for(const auto& thread : threads) {
            thread.wait();
        }
        for(const auto& thread : result) {
            for(const auto& file : thread) {
                out << "Matched in File: " << file.fileName << std::endl;
                for(const auto& match : file.lines) {
                    out << "    line num: " << match.first << " line: " << match.second << std::endl;
                }
            }
        }

    }
};

int main(int argc, char **argv) {
    std::ostringstream out;
    Searcher (argc,(const char**) argv).Run(out);
    std::cout << out.str();
    return 0;
}
