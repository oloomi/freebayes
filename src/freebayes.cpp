// 
// freebayes
//
// A bayesian genetic variant detector.
// 

// standard includes
//#include <cstdio>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <iterator>
#include <algorithm>
#include <cmath>
#include <time.h>
#include <float.h>

// private libraries
#include "BamReader.h"
#include "Fasta.h"
#include "TryCatch.h"
#include "Parameters.h"
#include "Allele.h"
#include "Sample.h"
#include "AlleleParser.h"
#include "Utility.h"

#include "multichoose.h"
#include "multipermute.h"

#include "Genotype.h"
#include "DataLikelihood.h"
#include "Marginals.h"
#include "ResultData.h"


// local helper debugging macros to improve code readability
#define DEBUG(msg) \
    if (parameters.debug) { cerr << msg << endl; }

// lower-priority messages
#ifdef VERBOSE_DEBUG
#define DEBUG2(msg) \
    if (parameters.debug2) { cerr << msg << endl; }
#else
#define DEBUG2(msg)
#endif

// must-see error messages
#define ERROR(msg) \
    cerr << msg << endl;


using namespace std; 


// freebayes main
int main (int argc, char *argv[]) {

    AlleleParser* parser = new AlleleParser(argc, argv);
    Parameters& parameters = parser->parameters;
    list<Allele*> alleles;

    Samples samples;

    ostream& out = *(parser->output);

    // this can be uncommented to force operation on a specific set of genotypes
    vector<Allele> allGenotypeAlleles;
    allGenotypeAlleles.push_back(genotypeAllele(ALLELE_GENOTYPE, "A", 1));
    allGenotypeAlleles.push_back(genotypeAllele(ALLELE_GENOTYPE, "T", 1));
    allGenotypeAlleles.push_back(genotypeAllele(ALLELE_GENOTYPE, "G", 1));
    allGenotypeAlleles.push_back(genotypeAllele(ALLELE_GENOTYPE, "C", 1));

    int allowedAlleleTypes = ALLELE_REFERENCE;
    if (parameters.allowSNPs) {
        allowedAlleleTypes |= ALLELE_SNP;
    }
    if (parameters.allowIndels) {
        allowedAlleleTypes |= ALLELE_INSERTION;
        allowedAlleleTypes |= ALLELE_DELETION;
    }
    if (parameters.allowMNPs) {
        allowedAlleleTypes |= ALLELE_MNP;
    }

    // output VCF header
    if (parameters.output == "vcf") {
        vcfHeader(out, parser->reference.filename, parser->sampleList, parameters, parser->sequencingTechnologies);
    }

    unsigned long total_sites = 0;
    unsigned long processed_sites = 0;

    while (parser->getNextAlleles(samples, allowedAlleleTypes)) {

        ++total_sites;

        DEBUG2("at start of main loop");

        // don't process non-ATGC's in the reference
        string cb = parser->currentReferenceBaseString();
        if (cb != "A" && cb != "T" && cb != "C" && cb != "G") {
            DEBUG2("current reference base is N");
            continue;
        }

        if (parameters.trace) {
            for (Samples::iterator s = samples.begin(); s != samples.end(); ++s) {
                const string& name = s->first;
                for (Sample::iterator g = s->second.begin(); g != s->second.end(); ++g) {
                    vector<Allele*>& group = g->second;
                    for (vector<Allele*>::iterator a = group.begin(); a != group.end(); ++a) {
                        Allele& allele = **a;
                        parser->traceFile << parser->currentSequenceName << "," << (long unsigned int) parser->currentPosition + 1  
                            << ",allele," << name << "," << allele.readID << "," << allele.base() << ","
                            << allele.currentQuality() << "," << allele.mapQuality << endl;
                    }
                }
            }
            DEBUG2("after trace generation");
        }

        if (!parser->inTarget()) {
            DEBUG("position: " << parser->currentSequenceName << ":" << (long unsigned int) parser->currentPosition + 1
                    << " is not inside any targets, skipping");
            continue;
        }

        int coverage = countAlleles(samples);

        DEBUG("position: " << parser->currentSequenceName << ":" << (long unsigned int) parser->currentPosition + 1 << " coverage: " << coverage);

        // skips 0-coverage regions
        if (coverage == 0) {
            DEBUG("no alleles left at this site after filtering");
            continue;
        } else if (coverage < parameters.minCoverage) {
            DEBUG("post-filtering coverage of " << coverage << " is less than --min-coverage of " << parameters.minCoverage);
            continue;
        }

        DEBUG2("coverage " << parser->currentSequenceName << ":" << parser->currentPosition << " == " << coverage);

        // establish a set of possible alternate alleles to evaluate at this location
        // only evaluate alleles with at least one supporting read with mapping
        // quality (MQL1) and base quality (BQL1)

        if (!sufficientAlternateObservations(samples, parameters.minAltCount, parameters.minAltFraction)) {
            DEBUG("insufficient alternate observations");
            continue;
        }

        map<string, vector<Allele*> > alleleGroups;
        groupAlleles(samples, alleleGroups);
        DEBUG2("grouped alleles by equivalence");

        int containedAlleleTypes = 0;
        for (map<string, vector<Allele*> >::iterator group = alleleGroups.begin(); group != alleleGroups.end(); ++group) {
            containedAlleleTypes |= group->second.front()->type;
        }

        // to ensure proper ordering of output stream
        vector<string> sampleListPlusRef;

        for (vector<string>::iterator s = parser->sampleList.begin(); s != parser->sampleList.end(); ++s) {
            sampleListPlusRef.push_back(*s);
        }
        if (parameters.useRefAllele)
            sampleListPlusRef.push_back(parser->currentSequenceName);

        vector<Allele> genotypeAlleles = parser->genotypeAlleles(alleleGroups, samples, allGenotypeAlleles);

        if (genotypeAlleles.size() <= 1) { // if we have only one viable alternate, we don't have evidence for variation at this site
            DEBUG2("no alternate genotype alleles passed filters at " << parser->currentSequenceName << ":" << parser->currentPosition);
            continue;
        }
        DEBUG("genotype alleles: " << genotypeAlleles);

        ++processed_sites;

        // for each possible ploidy in the dataset, generate all possible genotypes
        map<int, vector<Genotype> > genotypesByPloidy;

        for (Samples::iterator s = samples.begin(); s != samples.end(); ++s) {
            string const& name = s->first;
            int samplePloidy = parser->currentSamplePloidy(name);
            if (genotypesByPloidy.find(samplePloidy) == genotypesByPloidy.end()) {
                DEBUG2("generating all possible genotypes for " << samplePloidy);
                genotypesByPloidy[samplePloidy] = allPossibleGenotypes(samplePloidy, genotypeAlleles);
                DEBUG2("done");
            }
        }

        DEBUG2("generated all possible genotypes:");
        if (parameters.debug2) {
            for (map<int, vector<Genotype> >::iterator s = genotypesByPloidy.begin(); s != genotypesByPloidy.end(); ++s) {
                vector<Genotype>& genotypes = s->second;
                for (vector<Genotype>::iterator g = genotypes.begin(); g != genotypes.end(); ++g) {
                    DEBUG2(*g);
                }
            }
        }

        Results results;
        vector<vector<SampleDataLikelihood> > sampleDataLikelihoods;
        vector<vector<SampleDataLikelihood> > variantSampleDataLikelihoods;
        vector<vector<SampleDataLikelihood> > invariantSampleDataLikelihoods;

        DEBUG2("calculating data likelihoods");
        // calculate data likelihoods
        for (Samples::iterator s = samples.begin(); s != samples.end(); ++s) {

            string sampleName = s->first;
            Sample& sample = s->second;
            vector<Genotype>& genotypes = genotypesByPloidy[parser->currentSamplePloidy(sampleName)];
            vector<Genotype*> genotypesWithObs;
            for (vector<Genotype>::iterator g = genotypes.begin(); g != genotypes.end(); ++g) {
                if (parameters.excludePartiallyObservedGenotypes) {
                    if (g->sampleHasSupportingObservationsForAllAlleles(sample)) {
                        genotypesWithObs.push_back(&*g);
                    }
                } else if (parameters.excludeUnobservedGenotypes) {
                    if (g->sampleHasSupportingObservations(sample)) {
                        genotypesWithObs.push_back(&*g);
                    }
                } else {
                    genotypesWithObs.push_back(&*g);
                }
            }

            // skip this sample if we have no observations supporting any of the genotypes we are going to evaluate
            if (genotypesWithObs.empty()) {
                continue;
            }

            vector<pair<Genotype*, long double> > probs = probObservedAllelesGivenGenotypes(sample, genotypesWithObs, parameters.RDF, parameters.useMappingQuality);

            if (parameters.trace) {
                for (vector<pair<Genotype*, long double> >::iterator p = probs.begin(); p != probs.end(); ++p) {
                    parser->traceFile << parser->currentSequenceName << "," << (long unsigned int) parser->currentPosition + 1 << ","
                        << sampleName << ",likelihood," << *(p->first) << "," << p->second << endl;
                }
            }

            Result& sampleData = results[sampleName];
            sampleData.name = sampleName;
            sampleData.observations = &sample;
            for (vector<pair<Genotype*, long double> >::iterator p = probs.begin(); p != probs.end(); ++p) {
                sampleData.push_back(SampleDataLikelihood(sampleName, &sample, p->first, p->second, 0));
            }
            sortSampleDataLikelihoods(sampleData);

            //cout << exp(thisSampleDataLikelihoods.front().prob) << " - " << exp(thisSampleDataLikelihoods.at(1).prob) << endl;
            if (parameters.genotypeVariantThreshold != 0) {
                if (sampleData.size() > 1
                        && float2phred(1 - (exp(sampleData.front().prob) - exp(sampleData.at(1).prob)))
                            < parameters.genotypeVariantThreshold) {
                    //cout << "varying sample " << name << endl;
                    variantSampleDataLikelihoods.push_back(sampleData);
                } else {
                    invariantSampleDataLikelihoods.push_back(sampleData);
                }
            } else {
                variantSampleDataLikelihoods.push_back(sampleData);
            }
            sampleDataLikelihoods.push_back(sampleData);

        }
        
        DEBUG2("finished calculating data likelihoods");

        // this section is a hack to make output of trace identical to BamBayes trace
        // and also outputs the list of samples
        vector<bool> samplesWithData;
        if (parameters.trace) {
            parser->traceFile << parser->currentSequenceName << "," << (long unsigned int) parser->currentPosition + 1 << ",samples,";
            for (vector<string>::iterator s = sampleListPlusRef.begin(); s != sampleListPlusRef.end(); ++s) {
                if (parameters.trace) parser->traceFile << *s << ":";
                Results::iterator r = results.find(*s);
                if (r != results.end()) {
                    samplesWithData.push_back(true);
                } else {
                    samplesWithData.push_back(false);
                }
            }
            parser->traceFile << endl;
        }


        // calculate genotype combo likelihoods, integral over nearby genotypes
        // calculate marginals
        // and determine best genotype combination

        //DEBUG2("generating banded genotype combinations from " << genotypes.size() << " genotypes and " << sampleDataLikelihoods.size() << " sample genotypes");
        list<GenotypeCombo> genotypeCombos;

        if (parameters.expectationMaximization) {
            expectationMaximizationSearchIncludingAllHomozygousCombos(
                    genotypeCombos,
                    sampleDataLikelihoods,
                    variantSampleDataLikelihoods,
                    invariantSampleDataLikelihoods,
                    samples,
                    genotypeAlleles,
                    parameters.WB,
                    parameters.TB,
                    parameters.genotypeComboStepMax,
                    parameters.TH,
                    parameters.pooled,
                    parameters.permute,
                    parameters.hwePriors,
                    parameters.obsBinomialPriors,
                    parameters.alleleBalancePriors,
                    parameters.diffusionPriorScalar,
                    parameters.expectationMaximizationMaxIterations);
        } else {
            DEBUG2("generating banded genotype combinations");
            bandedGenotypeCombinationsIncludingAllHomozygousCombos(
                    genotypeCombos,
                    sampleDataLikelihoods,
                    variantSampleDataLikelihoods,
                    invariantSampleDataLikelihoods,
                    samples,
                    genotypeAlleles,
                    parameters.WB,
                    parameters.TB,
                    parameters.genotypeComboStepMax,
                    parameters.TH,
                    parameters.pooled,
                    parameters.permute,
                    parameters.hwePriors,
                    parameters.obsBinomialPriors,
                    parameters.alleleBalancePriors,
                    parameters.diffusionPriorScalar);
        }


        // add back the invariants to the combos

        Allele refAllele = genotypeAllele(ALLELE_REFERENCE, string(1, parser->currentReferenceBase), 1);

        // sort by the normalized datalikelihood + prior
        DEBUG2("sorting genotype combination likelihoods");
        GenotypeComboResultSorter gcrSorter;
        genotypeCombos.sort(gcrSorter);
        genotypeCombos.unique();

        // get posterior normalizer
        vector<long double> comboProbs;
        for (list<GenotypeCombo>::iterator gc = genotypeCombos.begin(); gc != genotypeCombos.end(); ++gc) {
            comboProbs.push_back(gc->posteriorProb);
        }
        long double posteriorNormalizer = logsumexp_probs(comboProbs);

        DEBUG2("got posterior normalizer");
        if (parameters.trace) {
            parser->traceFile << parser->currentSequenceName << "," 
                << (long unsigned int) parser->currentPosition + 1 << ",posterior_normalizer," << posteriorNormalizer << endl;
        }

        // we provide p(var|data), or the probability that the location has
        // variation between individuals relative to the probability that it
        // has no variation
        //
        // in other words:
        // p(var|d) = 1 - p(AA|d) - p(TT|d) - P(GG|d) - P(CC|d)
        //
        // the approach is go through all the homozygous combos
        // and then subtract this from 1... resolving p(var|d)

        long double pVar = 1.0;
        long double pHom = 0.0;

        bool hasHetCombo = false;
        bool bestOverallComboIsHet = false;
        GenotypeCombo* bestCombo = NULL;

        // calculates pvar and gets the best het combo
        for (list<GenotypeCombo>::iterator gc = genotypeCombos.begin(); gc != genotypeCombos.end(); ++gc) {
            if (gc->isHomozygous()) {
                pVar -= safe_exp(gc->posteriorProb - posteriorNormalizer);
                pHom += safe_exp(gc->posteriorProb - posteriorNormalizer);
            } else if (!hasHetCombo) { // get the first het combo
                bestCombo = &*gc;
                hasHetCombo = true;
                if (gc == genotypeCombos.begin()) {
                    bestOverallComboIsHet = true;
                }
            }
        }

        // if for some reason there are no het combos, use the first combo
        if (!hasHetCombo) {
            bestCombo = &genotypeCombos.front();
        }

        DEBUG2("calculated pVar");

        // get the best heteroz
        GenotypeCombo& bestGenotypeCombo = *bestCombo; // reference pointer swap

        if (parameters.trace) {
            for (list<GenotypeCombo>::iterator gc = genotypeCombos.begin(); gc != genotypeCombos.end(); ++gc) {
                vector<Genotype*> comboGenotypes;
                for (GenotypeCombo::iterator g = gc->begin(); g != gc->end(); ++g)
                    comboGenotypes.push_back((*g)->genotype);
                long double posteriorProb = gc->posteriorProb;
                long double dataLikelihoodln = gc->probObsGivenGenotypes;
                long double priorln = gc->posteriorProb;
                long double priorlnG_Af = gc->priorProbG_Af;
                long double priorlnAf = gc->priorProbAf;
                long double priorlnBin = gc->priorProbObservations;

                parser->traceFile << parser->currentSequenceName << "," << (long unsigned int) parser->currentPosition + 1 << ",genotypecombo,";

                int j = 0;
                GenotypeCombo::iterator i = gc->begin();
                for (vector<bool>::iterator d = samplesWithData.begin(); d != samplesWithData.end(); ++d) {
                    if (*d) {
                        parser->traceFile << IUPAC(*(*i)->genotype);
                        ++i;
                    } else {
                        parser->traceFile << "?";
                    }
                }
                // TODO cleanup this and above
                parser->traceFile 
                    << "," << dataLikelihoodln
                    << "," << priorln
                    << "," << priorlnG_Af
                    << "," << priorlnAf
                    << "," << priorlnBin
                    << "," << posteriorProb
                    << "," << safe_exp(posteriorProb - posteriorNormalizer)
                    << endl;
            }
        }

        DEBUG2("got bestAlleleSamplingProb");
        DEBUG("pVar = " << pVar << " " << parameters.PVL
              << " pHom = " << pHom
              << " 1 - pHom = " << 1 - pHom);

        if ((1 - pHom) >= parameters.PVL) {

            GenotypeCombo bestGenotypeComboByMarginals;

            if (parameters.calculateMarginals) {

                DEBUG2("calculating marginal likelihoods");

                // resample the posterior, this time without bounds on the
                // samples we vary, ensuring that we can generate marginals for
                // all sample/genotype combinations

                //SampleDataLikelihoods marginalLikelihoods = sampleDataLikelihoods;  // heavyweight copy...
                GenotypeCombo nullCombo;
                GenotypeCombo bestComboOrdered; // ordered by the samples in sampleDataLikelihoods
                GenotypeComboMap bestComboMap;
                orderedGenotypeCombo(
                        genotypeCombos.front(),
                        bestComboOrdered,
                        sampleDataLikelihoods,
                        parameters.TH,
                        true, // act as if pooled
                        parameters.permute,
                        true, // hwe priors
                        parameters.obsBinomialPriors,
                        parameters.alleleBalancePriors,
                        parameters.diffusionPriorScalar);

                for (int i = 0; i < parameters.genotypingMaxIterations; ++i) {

                    //cout << "iteration " << i << endl;
                    list<GenotypeCombo> localGenotypeCombos;
                    allLocalGenotypeCombinations(
                            localGenotypeCombos,
                            (i == 0 ? bestComboOrdered : nullCombo), // seed with the best combo on the first pass
                            sampleDataLikelihoods,
                            samples,
                            genotypeAlleles,
                            parameters.genotypeComboStepMax,
                            parameters.TH,
                            true, // act as if pooled
                            parameters.permute,
                            true, // hwe priors
                            parameters.obsBinomialPriors,
                            parameters.alleleBalancePriors,
                            parameters.diffusionPriorScalar);

                    // sort and remove any duplicates
                    localGenotypeCombos.sort(gcrSorter);
                    localGenotypeCombos.unique();

                    //SampleDataLikelihoods previousDataLikelihoods = sampleDataLikelihoods;  // heavyweight copy...

                    // estimate marginal genotype likelihoods, GQ in the VCF output
                    long double delta = marginalGenotypeLikelihoods(localGenotypeCombos, sampleDataLikelihoods);

                    //cout << "iteration " << i << " delta " << delta << endl;

                    // sort data likelihoods by marginal likelihoods
                    // and checks for convergence
                    if (!sortSampleDataLikelihoodsByMarginals(sampleDataLikelihoods)) {
                        break;
                    }

                    // debugging... print changes in sorting
                    /*
                    SampleDataLikelihoods::iterator s = sampleDataLikelihoods.begin();
                    for (SampleDataLikelihoods::iterator p = previousDataLikelihoods.begin(); p != previousDataLikelihoods.end(); ++p, ++s) {
                        if (s->front().genotype != p->front().genotype) {
                            cout << "swapped " << *p->front().genotype << " for " << *s->front().genotype << endl;
                        }
                    }
                    */

                    localGenotypeCombos.clear();
                    nullCombo.clear();

                }

                // generate the best marginal combo according to marginals, which we've sorted by
                dataLikelihoodMaxGenotypeCombo(
                        bestGenotypeComboByMarginals,
                        sampleDataLikelihoods,
                        parameters.TH,
                        parameters.pooled,
                        parameters.permute,
                        parameters.hwePriors,
                        parameters.obsBinomialPriors,
                        parameters.alleleBalancePriors,
                        parameters.diffusionPriorScalar);

                // store the marginal data likelihoods in the results, for easy parsing
                // like a vector -> map conversion...
                results.update(sampleDataLikelihoods);

            }

            string referenceBase(1, parser->currentReferenceBase);
            map<string, int> repeats;
            if (parameters.showReferenceRepeats) {
                repeats = parser->repeatCounts(12);
            }
            // get the unique alternate alleles in this combo, sorted by frequency in the combo
            vector<pair<Allele, int> > alternates = alternateAlleles(bestGenotypeCombo, referenceBase);
            if (parameters.reportAllAlternates) {
                for (vector<pair<Allele, int> >::iterator a = alternates.begin(); a != alternates.end(); ++a) {
                    Allele& alt = a->first;
                    out << vcf(pHom,
                            samples,
                            referenceBase,
                            alt.base(),
                            alt,
                            repeats,
                            parser->sampleList,
                            coverage,
                            (parameters.calculateMarginals ? bestGenotypeComboByMarginals : bestGenotypeCombo),
                            bestOverallComboIsHet,
                            alleleGroups,
                            genotypesByPloidy,
                            parser->sequencingTechnologies,
                            results,
                            parser)
                        << endl;
                }
            } else {
                Allele& bestAlt = alternates.front().first;
                // TODO update the vcf output function to handle the reporting of multiple alternate alleles
                out << vcf(pHom,
                        samples,
                        referenceBase,
                        bestAlt.base(),
                        bestAlt,
                        repeats,
                        parser->sampleList,
                        coverage,
                        (parameters.calculateMarginals ? bestGenotypeComboByMarginals : bestGenotypeCombo),
                        bestOverallComboIsHet,
                        alleleGroups,
                        genotypesByPloidy,
                        parser->sequencingTechnologies,
                        results,
                        parser)
                    << endl;
            }
        } else if (!parameters.failedFile.empty()) {
            // XXX don't repeat yourself
            // get the unique alternate alleles in this combo, sorted by frequency in the combo
            long unsigned int position = parser->currentPosition;
            for (vector<Allele>::iterator ga =  genotypeAlleles.begin(); ga != genotypeAlleles.end(); ++ga) {
                if (ga->type == ALLELE_REFERENCE)
                    continue;
                parser->failedFile
                    << parser->currentSequenceName << "\t"
                    << position << "\t"
                    << position + ga->length << "\t"
                    << *ga << endl;
            }
            // BED format
        }
        DEBUG2("finished position");

    }

    DEBUG("total sites: " << total_sites << endl
         << "processed sites: " << processed_sites << endl
         << "ratio: " << (float) processed_sites / (float) total_sites);

    delete parser;

    return 0;

}
