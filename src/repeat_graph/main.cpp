//(c) 2016 by Authors
//This file is a part of ABruijn program.
//Released under the BSD license (see LICENSE file)

#include <iostream>
#include <getopt.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

#include <execinfo.h>

#include "../sequence/vertex_index.h"
#include "../sequence/sequence_container.h"
#include "../sequence/overlap.h"
#include "../common/config.h"
#include "../common/logger.h"
#include "repeat_graph.h"
#include "multiplicity_inferer.h"
#include "graph_processing.h"
#include "repeat_resolver.h"
#include "output_generator.h"
#include "contig_extender.h"

bool parseArgs(int argc, char** argv, std::string& readsFasta, 
			   std::string& outFolder, std::string& logFile, 
			   std::string& inAssembly, int& kmerSize,
			   int& minOverlap, bool& debug, size_t& numThreads, 
			   std::string& configPath, bool& graphContinue)
{
	auto printUsage = [argv]()
	{
		std::cerr << "Usage: " << argv[0]
				  << "\tin_assembly reads_files out_folder config_path\n\t"
				  << "[-l log_file] [-t num_threads] [-v min_overlap]\n\t"
				  << "[-k kmer_size] [-d]\n\n"
				  << "positional arguments:\n"
				  << "\tin_assembly\tpath to input assembly\n"
				  << "\treads_files\tcomma-separated list with reads\n"
				  << "\tout_assembly\tpath to output assembly\n"
				  << "\tconfig_path\tpath to config file\n"
				  << "\noptional arguments:\n"
				  << "\t-k kmer_size\tk-mer size [default = 15] \n"
				  << "\t-v min_overlap\tminimum overlap between reads "
				  << "[default = 5000] \n"
				  << "\t-g \t\tcontinue contigs using graph structure "
				  << "[default = false] \n"
				  << "\t-d \t\tenable debug output "
				  << "[default = false] \n"
				  << "\t-l log_file\toutput log to file "
				  << "[default = not set] \n"
				  << "\t-t num_threads\tnumber of parallel threads "
				  << "[default = 1] \n";
	};

	numThreads = 1;
	debug = false;
	graphContinue = false;
	minOverlap = 5000;
	kmerSize = 15;

	const char optString[] = "l:t:k:v:hdg";
	int opt = 0;
	while ((opt = getopt(argc, argv, optString)) != -1)
	{
		switch(opt)
		{
		case 't':
			numThreads = atoi(optarg);
			break;
		case 'v':
			minOverlap = atoi(optarg);
			break;
		case 'k':
			kmerSize = atoi(optarg);
			break;
		case 'l':
			logFile = optarg;
			break;
		case 'd':
			debug = true;
			break;
		case 'g':
			graphContinue = true;
			break;
		case 'h':
			printUsage();
			exit(0);
		}
	}
	if (argc - optind != 4)
	{
		printUsage();
		return false;
	}

	inAssembly = *(argv + optind);
	readsFasta = *(argv + optind + 1);
	outFolder = *(argv + optind + 2);
	configPath = *(argv + optind + 3);

	return true;
}

bool fileExists(const std::string& path)
{
	std::ifstream fin(path);
	return fin.good();
}

void segfaultHandler(int signal)
{
	void *stackArray[20];
	size_t size = backtrace(stackArray, 10);
	Logger::get().error() << "Segmentation fault! Backtrace:";
	char** backtrace = backtrace_symbols(stackArray, size);
	for (size_t i = 0; i < size; ++i)
	{
		Logger::get().error() << "\t" << backtrace[i];
	}
	exit(1);
}

void exceptionHandler()
{
	static bool triedThrow = false;
	try
	{
        if (!triedThrow)
		{
			triedThrow = true;
			throw;
		}
    }
    catch (const std::exception &e) 
	{
        Logger::get().error() << "Caught unhandled exception: " << e.what();
    }
	catch (...) {}

	void *stackArray[20];
	size_t size = backtrace(stackArray, 10);
	char** backtrace = backtrace_symbols(stackArray, size);
	for (size_t i = 0; i < size; ++i)
	{
		Logger::get().error() << "\t" << backtrace[i];
	}
	exit(1);
}

int main(int argc, char** argv)
{
	#ifndef _DEBUG
	signal(SIGSEGV, segfaultHandler);
	std::set_terminate(exceptionHandler);
	#endif

	bool debugging = false;
	bool graphContinue = false;
	size_t numThreads = 1;
	int kmerSize = 15;
	int minOverlap = 5000;
	std::string readsFasta;
	std::string inAssembly;
	std::string outFolder;
	std::string logFile;
	std::string configPath;
	if (!parseArgs(argc, argv, readsFasta, outFolder, logFile, inAssembly,
				   kmerSize, minOverlap, debugging, 
				   numThreads, configPath, graphContinue))  return 1;
	Parameters::get().minimumOverlap = minOverlap;
	Parameters::get().kmerSize = kmerSize;
	Parameters::get().numThreads = numThreads;

	Logger::get().setDebugging(debugging);
	if (!logFile.empty()) Logger::get().setOutputFile(logFile);
	Logger::get().debug() << "Build date: " << __DATE__ << " " << __TIME__;
	std::ios::sync_with_stdio(false);
	Config::load(configPath);

	Logger::get().info() << "Reading sequences";
	SequenceContainer seqAssembly; 
	SequenceContainer seqReads;
	std::vector<std::string> readsList = splitString(readsFasta, ',');
	try
	{
		seqAssembly.loadFromFile(inAssembly);
		for (auto& readsFile : readsList)
		{
			seqReads.loadFromFile(readsFile);
		}
	}
	catch (SequenceContainer::ParseException& e)
	{
		Logger::get().error() << e.what();
		return 1;
	}

	RepeatGraph rg(seqAssembly);
	GraphProcessor proc(rg, seqAssembly, seqReads);
	ReadAligner aligner(rg, seqAssembly, seqReads);
	OutputGenerator outGen(rg, aligner, seqAssembly, seqReads);

	Logger::get().info() << "Building repeat graph";
	rg.build();
	outGen.outputDot(proc.getEdgesPaths(), outFolder + "/graph_raw.dot");
	proc.simplify();

	Logger::get().info() << "Aligning reads to the graph";
	aligner.alignReads();

	MultiplicityInferer multInf(rg, aligner, seqAssembly, seqReads);
	multInf.estimateCoverage();
	multInf.removeUnsupportedEdges();
	multInf.removeUnsupportedConnections();
	multInf.separateHaplotypes();

	Logger::get().info() << "Resolving repeats";
	RepeatResolver resolver(rg, seqAssembly, seqReads, aligner, multInf);
	resolver.findRepeats();

	outGen.outputDot(proc.getEdgesPaths(), outFolder + "/graph_before_rr.dot");
	outGen.outputGfa(proc.getEdgesPaths(), outFolder + "/graph_before_rr.gfa");
	outGen.outputFasta(proc.getEdgesPaths(), outFolder + "/graph_before_rr.fasta");

	resolver.resolveRepeats();
	resolver.fixLongEdges();
	outGen.outputDot(proc.getEdgesPaths(), outFolder + "/graph_after_rr.dot");

	Logger::get().info() << "Generating contigs";

	ContigExtender extender(rg, aligner, seqAssembly, seqReads, 
							multInf.getMeanCoverage());
	extender.generateUnbranchingPaths();
	extender.generateContigs(graphContinue);
	extender.outputContigs(outFolder + "/graph_paths.fasta");
	extender.outputStatsTable(outFolder + "/contigs_stats.txt");
	extender.outputScaffoldConnections(outFolder + "/scaffolds_links.txt");

	outGen.dumpRepeats(extender.getUnbranchingPaths(),
					   outFolder + "/repeats_dump.txt");
	outGen.outputDot(extender.getUnbranchingPaths(),
					 outFolder + "/graph_final.dot");
	outGen.outputFasta(extender.getUnbranchingPaths(),
					   outFolder + "/graph_final.fasta");
	outGen.outputGfa(extender.getUnbranchingPaths(),
					 outFolder + "/graph_final.gfa");
	return 0;
}
