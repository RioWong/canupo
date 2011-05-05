#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <algorithm>
#include <limits>

// graphics lib
#include <cairo/cairo.h>

#include <strings.h>
#include <math.h>

#include "points.hpp"
#include "base64.hpp"
#include "linearSVM.hpp"

using namespace std;

typedef LinearSVM::sample_type sample_type;

int help(const char* errmsg = 0) {
    if (errmsg) cout << "Error: " << errmsg << endl;
cout << "\
suggest_classifier outfile.svg [ msc(non label) ...] : class1.msc ... - class2.msc ...\n\
"<<endl;
        return 0;
}

bool fpeq(FloatType a, FloatType b) {
    static const FloatType epsilon = 1e-6;
    if (b==0) return fabs(a)<epsilon;
    FloatType ratio = a/b;
    return ratio>1-epsilon && ratio<1+epsilon;
}

// if vector is empty, fill it
// otherwise check the vectors match
int read_msc_header(ifstream& mscfile, vector<FloatType>& scales, int& ptnparams) {
    int npts;
    mscfile.read((char*)&npts,sizeof(npts));
    if (npts<=0) help("invalid file");
    
    int nscales_thisfile;
    mscfile.read((char*)&nscales_thisfile, sizeof(nscales_thisfile));
    vector<FloatType> scales_thisfile(nscales_thisfile);
    for (int si=0; si<nscales_thisfile; ++si) mscfile.read((char*)&scales_thisfile[si], sizeof(FloatType));
    if (nscales_thisfile<=0) help("invalid file");
    
    // all files must be consistant
    if (scales.size() == 0) {
        scales = scales_thisfile;
    } else {
        if (scales.size() != nscales_thisfile) {cerr<<"input file mismatch: "<<endl; return 1;}
        for (int si=0; si<scales.size(); ++si) if (!fpeq(scales[si],scales_thisfile[si])) {cerr<<"input file mismatch: "<<endl; return 1;}
    }
    
    // TODO: check consistency of ptnparams
    mscfile.read((char*)&ptnparams, sizeof(int));

    return npts;
}

void read_msc_data(ifstream& mscfile, int nscales, int npts, sample_type* data, int ptnparams) {
    for (int pt=0; pt<npts; ++pt) {
        // we do not care for the point coordinates and other parameters
        for (int i=0; i<ptnparams; ++i) {
            FloatType param;
            mscfile.read((char*)&param, sizeof(FloatType));
        }
        for (int s=0; s<nscales; ++s) {
            FloatType a,b;
            mscfile.read((char*)(&a), sizeof(FloatType));
            mscfile.read((char*)(&b), sizeof(FloatType));
            FloatType c = 1 - a - b;
            // project in the equilateral triangle a*(0,0) + b*(1,0) + c*(1/2,sqrt3/2)
            // equivalently to the triangle formed by the three components unit vector
            // (1,0,0), (0,1,0) and (0,0,1) when considering a,b,c in 3D
            // so each a,b,c = dimensionality of the data is given equal weight
            // is this necessary ? => not for linear classifiers, but plan ahead...
            FloatType x = b + c / 2;
            FloatType y = c * sqrt(3)/2;
            (*data)(s*2) = x;
            (*data)(s*2+1) = y;
        }
        // we do not care for number of neighbors and average dist between nearest neighbors
        // TODO: take this info into account to weight the samples and improve the classifier
        int fooi;
        for (int i=0; i<nscales; ++i) mscfile.read((char*)&fooi, sizeof(int));
/*        FloatType foof;
        for (int i=0; i<nscales; ++i) mscfile.read((char*)&foof, sizeof(FloatType));*/
        ++data;
    }
}

int ppmwrite(cairo_surface_t *surface, const char* filename) {
    int height = cairo_image_surface_get_height(surface);
    int width = cairo_image_surface_get_width(surface);
    int stride = cairo_image_surface_get_stride(surface);
    unsigned char* data = cairo_image_surface_get_data(surface);
    ofstream ppmfile(filename);
    ppmfile << "P3 " << width << " " << height << " " << 255 << endl;
    for (int row = 0; row<height; ++row) {
        for (int col = 0; col<width*4; col+=4) {
            ppmfile << (int)data[col+2] << " " << (int)data[col+1] << " " << (int)data[col+0] << " ";
        }
        data += stride;
    }
}

cairo_status_t png_copier(void *closure, const unsigned char *data, unsigned int length) {
    std::vector<char>* pngdata = (std::vector<char>*)closure;
    int cursize = pngdata->size();
    pngdata->resize(cursize + length); // use reserve() before, or this will be slow
    memcpy(&(*pngdata)[cursize], data, length);
    return CAIRO_STATUS_SUCCESS;
}

int main(int argc, char** argv) {
    
    if (argc<5) return help();
    
    ofstream svgfile(argv[1]);
    
    int arg_class1 = argc;
    for (int argi = 2; argi<argc; ++argi) if (!strcmp(argv[argi],":")) {
        arg_class1 = argi+1;
        break;
    }
    if (arg_class1>=argc) return help();
    
    int arg_class2 = argc;
    for (int argi = arg_class1+1; argi<argc; ++argi) if (!strcmp(argv[argi],"-")) {
        arg_class2 = argi+1;
        break;
    }
    if (arg_class2>=argc) return help();
    
    sample_type undefsample;
    int ptnparams;

    cout << "Loading unlabelled files" << endl;
    
    // neutral files, if any
    int ndata_unlabeled = 0;
    vector<FloatType> scales;
    for (int argi = 2; argi<arg_class1-1; ++argi) {
        ifstream mscfile(argv[argi], ifstream::binary);
        // read the file header
        int npts = read_msc_header(mscfile, scales, ptnparams);
        mscfile.close();
        ndata_unlabeled += npts;
    }
    int nscales = scales.size();
    int fdim = nscales * 2;
    if (nscales) undefsample.set_size(fdim,1);
    // fill data
    vector<sample_type> data_unlabeled(ndata_unlabeled, undefsample);
    int base_pt = 0;
    for (int argi = 2; argi<arg_class1-1; ++argi) {
        ifstream mscfile(argv[argi], ifstream::binary);
        // read the file header (again)
        int npts = read_msc_header(mscfile, scales, ptnparams);
        // read data
        read_msc_data(mscfile,nscales,npts,&data_unlabeled[base_pt], ptnparams);
        mscfile.close();
        base_pt += npts;
    }
    
    cout << "Loading class files" << endl;
    
    // class1 files
    int ndata_class1 = 0;
    for (int argi = arg_class1; argi<arg_class2-1; ++argi) {
        ifstream mscfile(argv[argi], ifstream::binary);
        int npts = read_msc_header(mscfile, scales, ptnparams);
        mscfile.close();
        ndata_class1 += npts;
    }
    // class2 files
    int ndata_class2 = 0;
    for (int argi = arg_class2; argi<argc; ++argi) {
        ifstream mscfile(argv[argi], ifstream::binary);
        int npts = read_msc_header(mscfile, scales, ptnparams);
        mscfile.close();
        ndata_class2 += npts;
    }
    nscales = scales.size(); // in case there is no unlabeled data
    fdim = nscales * 2;
    undefsample.set_size(fdim,1);
    int nsamples = ndata_class1+ndata_class2;
    vector<sample_type> samples(nsamples, undefsample);
    vector<FloatType> labels(nsamples, 0);
    for (int i=0; i<ndata_class1; ++i) labels[i] = -1;
    for (int i=ndata_class1; i<nsamples; ++i) labels[i] = 1;
    
    base_pt = 0;
    for (int argi = arg_class1; argi<arg_class2-1; ++argi) {
        ifstream mscfile(argv[argi], ifstream::binary);
        int npts = read_msc_header(mscfile, scales, ptnparams);
        read_msc_data(mscfile,nscales,npts,&samples[base_pt], ptnparams);
        mscfile.close();
        base_pt += npts;
    }
    for (int argi = arg_class2; argi<argc; ++argi) {
        ifstream mscfile(argv[argi], ifstream::binary);
        int npts = read_msc_header(mscfile, scales, ptnparams);
        read_msc_data(mscfile,nscales,npts,&samples[base_pt], ptnparams);
        mscfile.close();
        base_pt += npts;
    }
    
    cout << "Computing the two best projection directions" << endl;

    LinearSVM classifier;

    // shuffle before cross-validating to spread instances of each class
    dlib::randomize_samples(samples, labels);
    FloatType nu = classifier.crossValidate(10, samples, labels);
    cout << "Training" << endl;
    classifier.train(10, nu, samples, labels);
    
    // get the projections of each sample on the first classifier direction
    vector<FloatType> proj1(nsamples);
    for (int i=0; i<nsamples; ++i) proj1[i] = classifier.predict(samples[i]);
    
    // we now have the first hyperplane and corresponding decision boundary
    // projection onto the orthogonal subspace and repeat SVM to get a 2D plot
    // The procedure is a bit like PCA except we seek the successive directions of maximal
    // separability instead of maximal variance
    
    // project the data onto the hyperplane so as to get the second direction
    FloatType w2 = 0;
    for (int i=0; i<fdim; ++i) w2 += classifier.weights[i] * classifier.weights[i];
    for (int si=0; si<nsamples; ++si) {
        FloatType c = proj1[si] / w2;
        for(int i=0; i<fdim; ++i) samples[si](i) -= c * classifier.weights[i];
    }

    // already shuffled, and do not change order for the proj1 anyway
    LinearSVM ortho_classifier;
    nu = ortho_classifier.crossValidate(10, samples, labels);
    cout << "Training" << endl;
    ortho_classifier.train(10, nu, samples, labels);

    vector<FloatType> proj2(nsamples);
    for (int i=0; i<nsamples; ++i) proj2[i] = ortho_classifier.predict(samples[i]);

    // compute the reference points for orienting the classifier boundaries
    // pathological cases are possible where an arbitrary point in the (>0,>0)
    // quadrant is not in the +1 class for example
    // here, just use the mean of the classes
    Point refpt_pos(0,0,0);
    Point refpt_neg(0,0,0);
    for (int i=0; i<nsamples; ++i) {
        if (labels[i]>0) refpt_pos += Point(proj1[i], proj2[i], 1);
        else refpt_neg += Point(proj1[i], proj2[i], 1);
    }
    refpt_pos /= refpt_pos.z;
    refpt_neg /= refpt_neg.z;
    
    FloatType xming = numeric_limits<FloatType>::max();
    FloatType xmaxg = -numeric_limits<FloatType>::max();
    FloatType yming = numeric_limits<FloatType>::max();
    FloatType ymaxg = -numeric_limits<FloatType>::max();
    for (int i=0; i<data_unlabeled.size(); ++i) {
        FloatType x = classifier.predict(data_unlabeled[i]);
        FloatType y = ortho_classifier.predict(data_unlabeled[i]);
        xming = min(xming, x);
        xmaxg = max(xmaxg, x);
        yming = min(yming, y);
        ymaxg = max(ymaxg, y);
    }
    FloatType xminc = numeric_limits<FloatType>::max();
    FloatType xmaxc = -numeric_limits<FloatType>::max();
    FloatType yminc = numeric_limits<FloatType>::max();
    FloatType ymaxc = -numeric_limits<FloatType>::max();
    for (int i=0; i<nsamples; ++i) {
        xminc = min(xminc, proj1[i]);
        xmaxc = max(xmaxc, proj1[i]);
        yminc = min(yminc, proj2[i]);
        ymaxc = max(ymaxc, proj2[i]);
    }
    xming = min(xming, xminc);
    xmaxg = max(xmaxg, xmaxc);
    yming = min(yming, yminc);
    ymaxg = max(ymaxg, ymaxc);
    
    static const int svgSize = 800;
    static const int halfSvgSize = svgSize / 2;
    FloatType minX = numeric_limits<FloatType>::max();
    FloatType maxX = -minX;
    FloatType minY = minX;
    FloatType maxY = -minX;
    for (int i=0; i<nsamples; ++i) {
        minX = min(minX, proj1[i]);
        maxX = max(maxX, proj1[i]);
        minY = min(minY, proj2[i]);
        maxY = max(maxY, proj2[i]);
    }
    FloatType absmaxXY = fabs(max(max(max(-minX,maxX),-minY),maxY));
    FloatType scaleFactor = halfSvgSize / absmaxXY;
    
    PointCloud<Point2D> cloud2D;
    cloud2D.prepare(xming,xmaxg,yming,ymaxg,nsamples+data_unlabeled.size());
    for (int i=0; i<data_unlabeled.size(); ++i) cloud2D.insert(Point2D(
        classifier.predict(data_unlabeled[i]),
        ortho_classifier.predict(data_unlabeled[i])
    ));
    for (int i=0; i<nsamples; ++i) {
        cloud2D.insert(Point2D(proj1[i],proj2[i]));
    }
    
    FloatType absxymax = fabs(max(max(max(-xming,xmaxg),-yming),ymaxg));
    int nsearchpointm1 = 25;
    // radius from probabilistic SVM, diameter = 90% chance of correct classif
    FloatType radius = -log(1.0/0.9 - 1.0) / 2;
    
    int minsumd = numeric_limits<int>::max();
    FloatType minvx = 0, minvy = 0, minspcx = 0, minspcy = 0;
    
    cout << "Finding the line with least density" << flush;
    
    for (int spci = 0; spci <= nsearchpointm1; ++spci) {
        cout << "." << flush;
        
        FloatType spcx = refpt_neg.x + spci * (refpt_pos.x - refpt_neg.x) / nsearchpointm1;
        FloatType spcy = refpt_neg.y + spci * (refpt_pos.y - refpt_neg.y) / nsearchpointm1;
    
        // now we swipe a decision boundary in each direction around the point
        // and look for the lowest overall density along the boundary
        int nsearchdir = 90; // each 2 degree, as we swipe from 0 to 180 (unoriented lines)
        FloatType incr = max(xmaxg-xming, ymaxg-yming) / nsearchpointm1;
        vector<FloatType> sumds(nsearchdir);
#pragma omp parallel for
        for(int sd = 0; sd < nsearchdir; ++sd) {
            // use the parametric P = P0 + alpha*V formulation of a line
            // unit vector in the direction of the line
            FloatType vx = cos(M_PI * sd / nsearchdir);
            FloatType vy = sin(M_PI * sd / nsearchdir);
            sumds[sd] = 0;
            for(int sp = -nsearchpointm1/2; sp < nsearchpointm1/2; ++sp) {
                int s = sp * incr;
                FloatType x = vx * s + spcx;
                FloatType y = vy * s + spcy;
                vector<DistPoint<Point2D> > neighbors;
                cloud2D.findNeighbors(back_inserter(neighbors), Point2D(x,y), radius);
                sumds[sd] += neighbors.size();
            }
        }
        for(int sd = 0; sd < nsearchdir; ++sd) {
            if (sumds[sd]<minsumd) {
                minsumd = sumds[sd];
                minvx = cos(M_PI * sd / nsearchdir);
                minvy = sin(M_PI * sd / nsearchdir);
                minspcx = spcx;
                minspcy = spcy;
            }
        }
    }
    cout << endl;
    
    // so we finally have the decision boundary in this 2D space
    // P = P0 + alpha * V : px-p0x = alpha * vx  and  py-p0y = alpha * vy,
    // alpha = (px-p0x) / vx; // if vx is null see below
    // py-p0y = (px-p0x) * vy / vx
    // py = px * vy/vx + p0y - p0x * vy / vx
    // px * vy/vx - py + p0y - p0x * vy / vx = 0
    // equa: wx * px + wy * py + c = 0
    // with: wx = vy/vx; wy = -1; c = p0y - p0x * vy / vx
    // null vx just reverse roles as vy is then !=0 (v is unit vec)
    FloatType wx = 0, wy = 0, wc = 0;
    if (minvx!=0) { wx = minvy / minvx; wy = -1; wc = minspcy - minspcx * wx;}
    else {wx = -1; wy = minvx / minvy; wc = minspcx - minspcy * wy;}

    cout << "Drawing image" << endl;

    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, svgSize, svgSize);
    cairo_t *cr = cairo_create(surface);

    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_set_line_width(cr, 0);
    cairo_rectangle(cr, 0, 0, svgSize, svgSize);
    cairo_fill(cr);
    cairo_stroke(cr);
    
    cairo_set_line_width(cr, 1);
    // cumulating transluscent points to easily get a density estimate
    cairo_set_source_rgba(cr, 0.4, 0.4, 0.4, 0.1);
    // Plot points
    // first the unlabelled data, if any
    for (int i=0; i<ndata_unlabeled; ++i) {
        // we have to project this data as this was not done above
        FloatType x = classifier.predict(data_unlabeled[i]) * scaleFactor + halfSvgSize;
        FloatType y = halfSvgSize - ortho_classifier.predict(data_unlabeled[i]) * scaleFactor;
        cairo_arc(cr, x, y, 0.714, 0, 2*M_PI);
        cairo_stroke(cr);
    }
    // now plot the reference data. It is very well that it was randomised so we do not have one class on top of the other
    for (int i=0; i<nsamples; ++i) {
        FloatType x = proj1[i]*scaleFactor + halfSvgSize;
        FloatType y = halfSvgSize - proj2[i]*scaleFactor;
        if (labels[i]==1) cairo_set_source_rgba(cr, 1, 0, 0, 0.75);
        else cairo_set_source_rgba(cr, 0, 0, 1, 0.75);
        cairo_arc(cr, x, y, 0.714, 0, 2*M_PI);
        cairo_stroke(cr);
    }


/*  // probabilistic circles every 5 % proba of correct classification
    // too much, can't see anything in the middle
    // 1 / (1+exp(-d)) = pval = 0.55, 0.6, 0.65, 0.7, 0.75, 0.8, 0.85, 0.9, 0.95
    for (int i=5; i<=45; i+=5) {
        FloatType pval = (50.0 + i) / 100.0;
        // 1+exp(-d) = 1/pval
        // exp(-d) = 1/pval - 1
        FloatType d = -log(1.0/pval - 1.0);  // OK as pval<1
        cairo_arc(cr, halfSvgSize, halfSvgSize, d * scaleFactor, 0, 2*M_PI);
        cairo_stroke(cr);
    }

    // plot the circle at 95% proba of being correct (5% of being wrong)
    FloatType d95 = -log(1.0/0.95 - 1.0);
    cairo_arc(cr, halfSvgSize, halfSvgSize, d95 * scaleFactor, 0, 2*M_PI);
    cairo_stroke(cr);
*/
    // circles are prone to misinterpretation (radius = dist to hyperplane,
    // error not only in the center zone)
    // specify scales at the bottom-right of the image, in a less-used quadrant
    cairo_set_source_rgb(cr, 0.25,0.25,0.25);
    cairo_select_font_face (cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size (cr, 12);
    cairo_text_extents_t extents;
    FloatType dprob = -log(1.0/0.99 - 1.0) * scaleFactor;
    const char* text = "p(classif)>99%";
    cairo_text_extents(cr, text, &extents);
    cairo_move_to(cr, svgSize - dprob - 20 - extents.width - extents.x_bearing, svgSize - 15 - extents.height/2 - extents.y_bearing);
    cairo_show_text(cr, text);
    cairo_move_to(cr, svgSize - dprob - 10, svgSize - 15);
    cairo_line_to(cr, svgSize - 10, svgSize - 15);
    dprob = -log(1.0/0.95 - 1.0) * scaleFactor;
    text = "p(classif)>95%";
    cairo_text_extents(cr, text, &extents);
    cairo_move_to(cr, svgSize - dprob - 20 - extents.width - extents.x_bearing, svgSize - 35 - extents.height/2 - extents.y_bearing);
    cairo_show_text(cr, text);
    cairo_move_to(cr, svgSize - dprob - 10, svgSize - 35);
    cairo_line_to(cr, svgSize - 10, svgSize - 35);
    dprob = -log(1.0/0.9 - 1.0) * scaleFactor;
    text = "p(classif)>90%";
    cairo_text_extents(cr, text, &extents);
    cairo_move_to(cr, svgSize - dprob - 20 - extents.width - extents.x_bearing, svgSize - 55 - extents.height/2 - extents.y_bearing);
    cairo_show_text(cr, text);
    cairo_move_to(cr, svgSize - dprob - 10, svgSize - 55);
    cairo_line_to(cr, svgSize - 10, svgSize - 55);
    cairo_stroke(cr);

    // draw lines on top of points
    double dashes[2]; 
    dashes[0] = dashes[1] = svgSize*0.01;
    cairo_set_dash(cr, dashes, 2, svgSize*0.005);
    cairo_set_source_rgb(cr, 0.25,0.25,0.25);
    cairo_move_to(cr, 0,halfSvgSize);
    cairo_line_to(cr, svgSize,halfSvgSize);
    cairo_move_to(cr, halfSvgSize,0);
    cairo_line_to(cr, halfSvgSize,svgSize);
    cairo_stroke(cr);

    cout << "Writing the svg file" << endl;

    // output the svg file
    svgfile << "<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" width=\""<< svgSize << "\" height=\""<< svgSize <<"\" >" << endl;
    
    // Save the classifier parameters as an SVG comment so we can find them back later on
    // Use base64 encoded binary to preserve full precision
    
    vector<char> binary_parameters(
        sizeof(int)
      + nscales*sizeof(FloatType)
      + (fdim+1)*sizeof(FloatType)
      + (fdim+1)*sizeof(FloatType)
      + sizeof(FloatType)
      + sizeof(FloatType)
      + sizeof(int)
    );
    int bpidx = 0;
    memcpy(&binary_parameters[bpidx],&nscales,sizeof(int)); bpidx += sizeof(int);
    for (int i=0; i<nscales; ++i) {
        memcpy(&binary_parameters[bpidx],&scales[i],sizeof(FloatType));
        bpidx += sizeof(FloatType);
    }
    // Projections on the two 2D axis
    for (int i=0; i<=fdim; ++i) {
        memcpy(&binary_parameters[bpidx],&classifier.weights[i],sizeof(FloatType));
        bpidx += sizeof(FloatType);
    }
    for (int i=0; i<=fdim; ++i) {
        memcpy(&binary_parameters[bpidx],&ortho_classifier.weights[i],sizeof(FloatType));
        bpidx += sizeof(FloatType);
    }
    // boundaries
    memcpy(&binary_parameters[bpidx],&absmaxXY,sizeof(FloatType)); bpidx += sizeof(FloatType);
    // conversion from svg to 2D space
    memcpy(&binary_parameters[bpidx],&scaleFactor,sizeof(FloatType)); bpidx += sizeof(FloatType);
    memcpy(&binary_parameters[bpidx],&halfSvgSize,sizeof(int)); bpidx += sizeof(int);

    base64 codec;
    int nbytes;
    
    std::vector<char> base64commentdata(codec.get_max_encoded_size(binary_parameters.size()));
    nbytes = codec.encode(&binary_parameters[0], binary_parameters.size(), &base64commentdata[0]);
    nbytes += codec.encode_end(&base64commentdata[nbytes]);
    
    // comments work well and do not introduce any artifact in the resulting SVG
    // but sometimes they are not preserved... use a hidden text then as workaround
#ifdef CANUPO_NO_SVG_COMMENT
    svgfile << "<text style=\"font-size:1px;fill:#ffffff;fill-opacity:0;stroke:none\" x=\"20\" y=\"20\">params=" << &base64commentdata[0] << "</text>" << endl;
#else
    svgfile << "<!-- params " << &base64commentdata[0] << " -->" << endl;
#endif

#ifdef CANUPO_NO_PNG
    string filename = argv[1];
    filename.replace(filename.size()-3,3,"ppm");
    ppmwrite(surface,filename.c_str());
    svgfile << "<image xlink:href=\""<< filename << "\" width=\""<<svgSize<<"\" height=\""<<svgSize<<"\" x=\"0\" y=\"0\" style=\"z-index:0\" />" << endl;
#else
    //cairo_surface_write_to_png (surface, argv[1]);
    std::vector<char> pngdata;
    pngdata.reserve(800*800*3); // need only large enough init size
    cairo_surface_write_to_png_stream(surface, png_copier, &pngdata);

    // encode the png data into base64
    std::vector<char> base64pngdata(codec.get_max_encoded_size(pngdata.size()));
    codec.reset_encoder();
    nbytes = codec.encode(&pngdata[0], pngdata.size(), &base64pngdata[0]);
    nbytes += codec.encode_end(&base64pngdata[nbytes]);
    
    // include the image inline    
    svgfile << "<image xlink:href=\"data:image/png;base64,"<< &base64pngdata[0]
            << "\" width=\""<<svgSize<<"\" height=\""<<svgSize<<"\" x=\"0\" y=\"0\" style=\"z-index:0\" />" << endl;
#endif
    
    // include the reference points
    svgfile << "<circle cx=\""<< (refpt_pos.x*scaleFactor+halfSvgSize) <<"\" cy=\""<< (halfSvgSize-refpt_pos.y*scaleFactor) <<"\" r=\"2\" style=\"fill:none;stroke:#000000;stroke-width:1px;z-index:1;\" />" << endl;
    svgfile << "<circle cx=\""<< (refpt_neg.x*scaleFactor+halfSvgSize) <<"\" cy=\""<< (halfSvgSize-refpt_neg.y*scaleFactor) <<"\" r=\"2\" style=\"fill:none;stroke:#000000;stroke-width:1px;z-index:1;\" />" << endl;

    // plot decision boundary as a path
    // xy space in plane => scale and then reverse
    // first find homogeneous equa in the 2D space
    // convert the decision boundary to SVG space
    // ori: wx * x + wy * y + wc = 0
    // xsvg = x * scaleFactor + halfSvgSize; => x = (xsvg - halfSvgSize)  / scaleFactor
    // ysvg = halfSvgSize - y * scaleFactor; => y = (halfSvgSize - ysvg)  / scaleFactor
    // wxsvg * xsvg + wysvg * ysvg + csvg = 0
    // wx * x + wy * y + wc = 0
    // wx * (xsvg - halfSvgSize)  / scaleFactor + wy * (halfSvgSize - ysvg)  / scaleFactor + wc = 0
    // wx * (xsvg - halfSvgSize) + wy * (halfSvgSize - ysvg) + wc * scaleFactor = 0
    FloatType wxsvg = wx;
    FloatType wysvg = -wy;
    FloatType csvg = (wy-wx)*halfSvgSize + wc * scaleFactor;
    FloatType minspcxsvg = minspcx * scaleFactor + halfSvgSize;
    FloatType minspcysvg = halfSvgSize - minspcy * scaleFactor;
    // now intersect to find xminsvg, yminsvg, and so on
    // some may be NaN
    FloatType xsvgy0 = -csvg / wxsvg; // at ysvg = 0
    FloatType ysvgx0 = -csvg / wysvg; // at xsvg = 0
    FloatType xsvgymax = (-csvg -wysvg*svgSize) / wxsvg; // at ysvg = svgSize
    FloatType ysvgxmax = (-csvg -wxsvg*svgSize) / wysvg; // at xsvg = svgSize
    // NaN comparisons always fail, so use only positive tests and this is OK
    bool useLeft = (ysvgx0 >= 0) && (ysvgx0 <= svgSize);
    bool useRight = (ysvgxmax >= 0) && (ysvgxmax <= svgSize);
    bool useTop = (xsvgy0 >= 0) && (xsvgy0 <= svgSize);
    bool useBottom = (xsvgymax >= 0) && (xsvgymax <= svgSize);
    int sidescount = useLeft + useRight + useTop + useBottom;
    vector<Point2D> path;
//    if (sidescount==2) {
        svgfile << "<path style=\"fill:none;stroke:#000000;stroke-width:1px;z-index:1;\" d=\"M ";
        if (useLeft) {
            svgfile << 0 << "," << ysvgx0 << " L " << minspcxsvg<<","<<minspcysvg<<" L ";
            if (useTop) svgfile << xsvgy0 << "," << 0 << " ";
            if (useRight) svgfile << svgSize << "," << ysvgxmax << " ";
            if (useBottom) svgfile << xsvgymax << "," << svgSize << " ";
        }
        if (useTop) {
            svgfile << xsvgy0 << "," << 0 << " L " << minspcxsvg<<","<<minspcysvg<<" L ";
            if (useRight) svgfile << svgSize << "," << ysvgxmax << " ";
            if (useBottom) svgfile << xsvgymax << "," << svgSize << " ";
        }
        svgfile << "\" />" << endl;
//    }

    svgfile << "</svg>" << endl;
    svgfile.close();

    cairo_surface_destroy(surface);
    cairo_destroy(cr);

    return 0;
}
