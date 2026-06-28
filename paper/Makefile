# Build the interim whitepaper. pdflatex-only (no bibtex, no latexmk).
# The bibliography is embedded with \begin{thebibliography}; run pdflatex
# twice so cross-references and \cite labels resolve.
#
#   make          # builds carmix.pdf
#   make clean    # removes build outputs (PDF and aux files)
#
# Manual equivalent:
#   pdflatex carmix.tex && pdflatex carmix.tex

PDFLATEX ?= pdflatex
DOC := carmix

.PHONY: all clean

all: $(DOC).pdf

$(DOC).pdf: $(DOC).tex
	$(PDFLATEX) -interaction=nonstopmode -halt-on-error $(DOC).tex
	$(PDFLATEX) -interaction=nonstopmode -halt-on-error $(DOC).tex

clean:
	rm -f $(DOC).pdf $(DOC).aux $(DOC).log $(DOC).out $(DOC).toc
