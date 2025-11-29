#!/bin/zsh
cd ..

# Populate git summary
mkdir -p docs/data

line_count=$(git ls-files | xargs cat | wc -l)

start_time=$(git log --reverse --format="%ad" --date=format:'%Y年%m月%d日' | head -n 1)
end_time=$(git log -1 --format="%ad" --date=format:'%Y年%m月%d日')

commit_count=$(git rev-list --count HEAD)

echo "这个操作系统共计 ${line_count} 行代码，从 ${start_time} 开始，至 ${end_time} 结束，共有 ${commit_count} 个commit" > docs/data/git_summary.tex

# Make the final build
cd docs
bibtex design
xelatex -shell-escape design.tex 
