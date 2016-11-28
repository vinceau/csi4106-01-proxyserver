TARGET = 2016840200.md
OUTPUT = 2016840200.pdf

all: build open

build: $(TARGET)
	pandoc $(TARGET) \
	    --filter pandoc-citeproc \
	    -S \
	    -o $(OUTPUT)

serve:
	ls *.md *.bib | entr make build

open:
	open $(OUTPUT)

clean:
	rm $(OUTPUT)

