/*
 * test_localizer.cpp
 *
 *  Created on: Aug 29, 2018
 *      Author: sujiwo
 */



#include <vector>
#include <string>
#include <iostream>
#include <iomanip>
#include <fstream>
#include <histedit.h>
#include <unistd.h>
#include <editline/readline.h>
#include <opencv2/highgui.hpp>
#include <pcl/io/pcd_io.h>
#include <pcl/point_types.h>

#include <boost/filesystem.hpp>

#include "VMap.h"
#include "KeyFrame.h"
#include "ImageDatabase.h"
#include "SequenceSLAM.h"
#include "Localizer.h"
#include "MapBuilder2.h"
#include "Viewer.h"
#include "datasets/OxfordDataset.h"
#include "datasets/MeidaiBagDataset.h"
#include "utilities.h"


using namespace std;
namespace fs = boost::filesystem;


struct {
	std::string velodyneCalibrationPath;
	std::string pcdMapPath;

	// XXX: Find a way to specify these values from external input
	TTransform lidarToCamera = defaultLidarToCameraTransform;

} meidaiNdtParameters;


// XXX: Find a way to specify these values from external input
CameraPinholeParams meidaiCamera1Params(
	1150.96938467,	// fx
	1150.96938467,	// fy
	988.511326762,	// cx
	692.803953253,	// cy
	1920,			// width
	1440			// height
);


typedef vector<string> stringTokens;


const string TestPrompt = "OX>";


class LineEditor
{
public:
	LineEditor(const char *argv0, const string &askPrompt="")
	{
		prompt = askPrompt + ' ';
		el = el_init("test_localizer", stdin, stdout, stderr);
		rl_initialize();
		tokenizer = tok_init(NULL);
	}


	~LineEditor()
	{
		tok_end(tokenizer);
		el_end(el);
	}


	stringTokens parseLine (const char *sline)
	{
		int nt;
		const char **stoklist;
		tok_str(tokenizer, sline, &nt, &stoklist);

		stringTokens st;
		for (int i=0; i<nt; i++) {
			string s(stoklist[i]);
			st.push_back(s);
		}
		tok_reset(tokenizer);

//		free(stoklist);
		return st;
	}


	stringTokens getLine ()
	{
		char *sline = readline(prompt.c_str());
		stringTokens st = parseLine(sline);
		free(sline);

		return st;
	}

protected:
	EditLine *el;
	Tokenizer *tokenizer;
	string prompt;
};


int localize_seq_slam (SequenceSLAM *seqSl, OxfordImagePreprocessor &proc, const string &imgPath)
{
	cv::Mat image = proc.load(imgPath);
	cv::imwrite("/tmp/1.png", image);
	exit(-1);

	vector<cv::Mat> imgList;
	imgList.push_back(image);

	seqSl->find(imgList, 10);
}




class VmmlCliApp
{
public:
	enum datasetType {
		OXFORD_DATASET_TYPE,
		MEIDAI_DATASET_TYPE
	} ;


	VmmlCliApp (int argc, char *argv[]):
		mLineEditor(argv[0], TestPrompt),
		mapPath("")

	{
	//	localizer = new Localizer()
	}


	~VmmlCliApp ()
	{
		if (mapSrc)
			delete(mapSrc);
		if (localizer)
			delete(localizer);
	}


	void processCommand (const stringTokens &command)
	{
		if (command[0][0]=='#' or command[0].size()==0)
			return;

		else if (command[0]=="map")
			{ RecordRuntime("MapOpen", map_open_cmd(command[1]) ); }

		else if (command[0]=="dataset")
			{ RecordRuntime("DatasetOpen", dataset_open_cmd(command[1], command[2])); }

		else if (command[0]=="map_pcl")
			map_dump_pcl();

		else if (command[0]=="dataset_trajectory")
			dataset_trajectory_dump(command[1]);

		else if (command[0]=="map_trajectory")
			map_trajectory_dump();

//			else if (command[0]=="find")
//				map_find_cmd(command[1]);

		else if (command[0]=="save")
			dataset_save_dsecond(command[1]);

		else if (command[0]=="savei")
			dataset_save_id(command[1]);

		else if (command[0]=="zoom")
			dataset_set_zoom(command[1]);

//			else if (command[0]=="dataset_simulate_seqslam")
//				dataset_simulate_seqslam(command[1]);

		else if (command[0]=="dataset_view")
			dataset_view(command[1]);

		else if (command[0]=="detect")
			{ RecordRuntime("PlaceDetection", map_detect_cmd(command[1]) ); }

		// To ask a subset, specify `start' and `stop' offset from beginning
		// as optional parameters
		else if (command[0]=="map_create")
			{ RecordRuntime("MapCreate", map_create_cmd(stringTokens(command.begin()+1, command.end())) ); }

		else if (command[0]=="map_info")
			map_info_cmd();

		else if (command[0]=="dataset_info")
			dataset_info_cmd();

		else if (command[0]=="mask")
			mask_set_cmd(command[1]);

		else if (command[0]=="velodyne" or command[0]=="pcdmap")
			dataset_set_param(command);

		else if (command[0]=="build")
			{ RecordRuntime("DatasetBuild", dataset_build(command) ); }

		else if (command[0]=="map_images")
			map_dump_images();

		else if (command[0]=="cd" or command[0]=="chdir")
			changeWorkingDirectory(command[1]);
	}


	static void fromScript(int argc, char *argv[])
	{
		VmmlCliApp scriptIterpreter (argc, argv);
		string scriptName(argv[1]);

		std::ifstream fd;
		fd.open(scriptName);
		if (fd.good()==false)
			throw std::runtime_error("Unable to open file "+scriptName);

		while(fd.eof()==false) {
			std::string curLine;
			std::getline(fd, curLine);

			if (curLine[0]=='#')
				continue;

			stringTokens command = scriptIterpreter.mLineEditor.parseLine(curLine.c_str());
			scriptIterpreter.processCommand(command);
		}

		cerr << "\nDone\n";
	}


	void loop()
	{
		bool doExit=false;
		while (doExit==false) {

			stringTokens command = mLineEditor.getLine();

			if (command[0]=="quit")
				break;

			processCommand(command);
		}
	}


protected:
	LineEditor mLineEditor;

	VMap *mapSrc = NULL;
	boost::filesystem::path mapPath;

	ImageDatabase *imgDb = NULL;
	SequenceSLAM *seqSlProv = NULL;
	Localizer *localizer = NULL;

	datasetType slDatasourceType;
	GenericDataset::Ptr loadedDataset;
	MeidaiBagDataset::Ptr meidaiDsPtr = nullptr;
	OxfordDataset::Ptr oxfordDsPtr = nullptr;
	boost::filesystem::path datasetPath;

	cv::Mat mask;


private:


	void changeWorkingDirectory (const string &toDir)
	{
		chdir(toDir.c_str());
		return;
	}


	void map_open_cmd(const string &mapPathInput)
	{
		try {
			mapSrc = new VMap();
			mapSrc->load(mapPathInput);
			localizer = new Localizer(mapSrc);
			localizer->setCameraParameterFromId(0);

			debug("Map loaded");
			imgDb = mapSrc->getImageDB();
			seqSlProv = imgDb->getSequence();
			mapPath = boost::filesystem::path(mapPathInput);

			mask = mapSrc->getMask();
			if (mask.empty()==false) {
				debug ("Map contains mask image file");
				localizer->setMask(mask);
			}

		} catch (exception &e) {
			debug ("Unable to load map");
		}
	}


	const string viewerWindowName="Dataset Viewer";
	void dataset_view(const string &durationSecStr)
	{
		cv::namedWindow(viewerWindowName);
		double d = std::stod(durationSecStr);
		auto di = loadedDataset->atDurationSecond(d);
		cv::Mat img = di->getImage();
		cv::cvtColor(img, img, CV_RGB2GRAY);
		cv::imshow(viewerWindowName, img);
		cv::waitKey(1);
	}

	const string mapDumpPcl = "/tmp/map.pcd";
	void map_dump_pcl()
	{
		auto pclDump = mapSrc->dumpPointCloudFromMapPoints();
		pcl::io::savePCDFileBinary(mapDumpPcl, *pclDump);
		debug("Point Cloud Map dumped to "+mapDumpPcl);
	}


	/*
	 * Building a `ground truth' for Meidai dataset accepts start and stop time,
	 * expressed in seconds after recording started
	 */
	void dataset_build(const stringTokens &cmd)
	{
		if (slDatasourceType!=MEIDAI_DATASET_TYPE) {
			debug("Oxford datasets need not to be built");
			return;
		}

		if (meidaiNdtParameters.pcdMapPath.empty()) {
			debug ("Point cloud map must be set with commands `pcdmap'");
			return;
		}

		string velodyneCalibrationPath;
		if (meidaiNdtParameters.velodyneCalibrationPath.empty()) {
			boost::filesystem::path myPath = getMyPath();
			myPath /= "params/meidai-64e-S2.yaml";
			velodyneCalibrationPath = myPath.string();
		}
		else
			velodyneCalibrationPath = meidaiNdtParameters.velodyneCalibrationPath;

		meidaiNdtParameters.lidarToCamera = defaultLidarToCameraTransform;
		meidaiDsPtr->setLidarParameters(meidaiNdtParameters.velodyneCalibrationPath, meidaiNdtParameters.pcdMapPath, meidaiNdtParameters.lidarToCamera);

		bool useLidar=true;
		if (cmd.size() == 2) {
			if (cmd[1]=="gnss")
				useLidar = false;
		}

		else if (cmd.size()>=3) {

			double startPos = stod(cmd[1]),
				stopPos = stod(cmd[2]);

			if (cmd.size()==4 and cmd[3]=="gnss")
				useLidar = false;

			debug ("Building from "+to_string(startPos) + " to " + to_string(stopPos));
			auto
				t1 = meidaiDsPtr->timeFromStart(startPos),
				t2 = meidaiDsPtr->timeFromStart(stopPos);
			meidaiDsPtr->setTimeConstraint(t1, t2);
		}

		if (useLidar==false)
			debug ("Not using NDT; camera positions are estimated from GNSS");

//		meidaiDsPtr->fo
/*
		if (slDatasourceType==MEIDAI_DATASET_TYPE) {
			if (meidaiNdtParameters.pcdMapPath.empty() or meidaiNdtParameters.velodyneCalibrationPath.empty()) {
				debug ("Parameters must be set with commands `velodyne' and `pcdmap'");
				return;
			}

			MeidaiBagDataset::Ptr nuDataset;
			bool resetSubset;
			bool useNdt = true;

			if (cmd.size()<=2) {
				nuDataset = static_pointer_cast<MeidaiBagDataset>(loadedDataset);
				resetSubset = true;
				if (cmd.size()==2 and cmd[1]=="gnss")
					useNdt = false;
			}

			else {

				double startPos = stod(cmd[1]),
					stopPos = stod(cmd[2]);

				if (cmd.size()==4 and cmd[3]=="gnss")
					useNdt = false;

				debug ("Building from "+to_string(startPos) + " to " + to_string(stopPos));
				MeidaiBagDataset::Ptr nTmp = static_pointer_cast<MeidaiBagDataset>(loadedDataset);
				nuDataset = nTmp->subset(startPos, stopPos);
				resetSubset = false;
			}

			if (useNdt==false)
				debug ("Not using NDT; camera positions are estimated from GNSS");

			meidaiNdtParameters.lidarToCamera = defaultLidarToCameraTransform;
			nuDataset->setLidarParameters(meidaiNdtParameters.velodyneCalibrationPath, meidaiNdtParameters.pcdMapPath, meidaiNdtParameters.lidarToCamera);
			nuDataset->forceCreateCache(resetSubset, useNdt);
		}

		else {
			debug("Oxford datasets need not to be built");
		}
*/
	}


	void map_info_cmd()
	{
		if (mapSrc==NULL) {
			debug("Map not loaded");
			return;
		}
		debug("# of keyframe(s): "+to_string(mapSrc->numOfKeyFrames()));
		debug("# of map point(s): " +to_string(mapSrc->numOfMapPoints()));

		auto camInfo = mapSrc->getCameraParameter(0);
		debug("Horizontal FieldOfView (rad): " + to_string(camInfo.getHorizontalFoV()));
		debug("Vertical FieldOfView (rad): " + to_string(camInfo.getVerticalFoV()));

		auto &mapInfo = mapSrc->getAllInfo();
		for (auto rInfo: mapInfo) {
			const string
				&k = rInfo.first,
				&v = rInfo.second;
			stringstream ss;
			ss << k << " : " << v;
			debug(ss.str());
		}
	}


	void dataset_info_cmd()
	{
		if (slDatasourceType==MEIDAI_DATASET_TYPE) {
			MeidaiBagDataset::Ptr meidaiDs = static_pointer_cast<MeidaiBagDataset>(loadedDataset);
			auto cameraTrack = meidaiDs->getCameraTrajectory();

			if (meidaiDs->isCameraTrajectoryComplete()) {
				debug("Camera trajectory is complete");
			}

			else {
				debug ("Dataset does not contain camera trajectory; must be built");
			}
		}

		debug("# of images:" + to_string(loadedDataset->size()));

		debug("Frequency (Hz): " + to_string(loadedDataset->hertz()));

		debug("Duration (sec): " + to_string(loadedDataset->length()));
	}


	const string dumpMapTrajectoryPath = "/tmp/dump_map_trajectory.csv";
	void map_trajectory_dump()
	{
		fstream mapTrFd (dumpMapTrajectoryPath, ios_base::out|ios_base::trunc);
		if (!mapTrFd.is_open()) {
			debug("Unable to create "+dumpMapTrajectoryPath);
			return;
		}

		auto mapPoses = mapSrc->dumpCameraPoses();
		uint32_t ix = 0;
		for (auto ps: mapPoses) {
			mapTrFd << ix << " ";
			mapTrFd << dumpVector(ps.first) << " " << dumpVector(ps.second) << endl;
			ix += 1;
		}

		mapTrFd.close();
		debug("Map trajectory dumped to "+dumpMapTrajectoryPath);
	}


	const string dumpDatasetTrajectoryPath = "/tmp/dump_dataset_trajectory";
	void dataset_trajectory_dump(const string &type="camera")
	{
		Trajectory dsTrack;
		string dumpPathName;

		if (slDatasourceType==MEIDAI_DATASET_TYPE) {

			if (type=="gnss")
				dsTrack = meidaiDsPtr->getGnssTrajectory();
			else if (type=="ndt")
				dsTrack = meidaiDsPtr->getNdtTrajectory();
			else if (type=="camera")
				dsTrack = meidaiDsPtr->getCameraTrajectory();
			else {
				debug ("Unknown trajectory type for Meidai Dataset");
				return;
			}

			dumpPathName = dumpMapTrajectoryPath + '-' + type + ".csv";
		}

		else if(slDatasourceType==OXFORD_DATASET_TYPE) {
			dsTrack = loadedDataset->getCameraTrajectory();
			dumpPathName = dumpDatasetTrajectoryPath + '-' + fs::basename(oxfordDsPtr->getPath());
		}

		dsTrack.dump(dumpPathName);
		debug("Dataset trajectory dumped to "+dumpPathName);
		return;
	}


	void dataset_open_cmd(const string &dsPath, const string &modelDir)
	{
		datasetPath = boost::filesystem::path (dsPath);

		if (boost::filesystem::is_directory(datasetPath)) {
			loadedDataset = OxfordDataset::load(datasetPath.string(), modelDir);
			slDatasourceType = OXFORD_DATASET_TYPE;
			oxfordDsPtr = static_pointer_cast<OxfordDataset> (loadedDataset);
			debug ("Oxford-type Dataset Loaded");
		}

		else if (datasetPath.extension()==".bag") {
			loadedDataset = MeidaiBagDataset::load(datasetPath.string());
			meidaiDsPtr = static_pointer_cast<MeidaiBagDataset>(loadedDataset);
			slDatasourceType = MEIDAI_DATASET_TYPE;
			debug ("Nagoya University Dataset Loaded");
		}

		else {
			debug("Unsupported dataset type");
			return;
		}
	}


	void dataset_set_param(const stringTokens &command)
	{
		if (slDatasourceType==MEIDAI_DATASET_TYPE) {
			if (command[0]=="velodyne")
				meidaiNdtParameters.velodyneCalibrationPath = command[1];
			else if (command[0]=="pcdmap")
				meidaiNdtParameters.pcdMapPath = command[1];
		}
	}


	/*
	 * You can feed an offset number from dataset, or a path to an image
	 */
	void map_detect_cmd(const string &detectionStr)
	{
		if (localizer==NULL) {
			debug("Map not loaded");
			return;
		}

		cv::Mat img;

		if (boost::filesystem::exists(boost::filesystem::path(detectionStr))) {
			img = cv::imread(detectionStr, cv::IMREAD_COLOR);
			if (img.empty()) {
				debug ("Unable to open requested file");
				return;
			}
		}

		else {
			double d = std::stod(detectionStr);
			auto di = loadedDataset->atDurationSecond(d);
			img = di->getImage();
		}

		cv::cvtColor(img, img, CV_RGB2GRAY);
		kfid kmap;
		Pose computedPose;
		bool gotplace = localizer->detect_mt(img, kmap, computedPose);

		debug("Detecting " + detectionStr);
		if (gotplace) {
			debug(computedPose.str(true));
			debug("Result: "+to_string(kmap));
		}
		else {
			debug ("Unable to detect place");
		}
	}

	const string dumpImagePath = "/tmp/dump_image.png";

	void dataset_save_dsecond(const string &durationSecStr)
	{
		double d = std::stod(durationSecStr);
		auto di = loadedDataset->atDurationSecond(d);
		cv::Mat img = di->getImage();
		cv::imwrite(dumpImagePath, img);
		debug("Dumped to " + dumpImagePath);
	}

	void dataset_save_id(const string &sid)
	{
		dataItemId requestId = static_cast<dataItemId>(std::stoi(sid));

		auto md = loadedDataset->get(requestId);
		cv::Mat img = md->getImage();
		cv::imwrite(dumpImagePath, img);
		debug("Image #" + sid + " dumped to " + dumpImagePath);
	}

	void debug(const string &s, double is_error=false)
	{
		if (!is_error)
			cerr << s << endl << flush;
		else
			cout << s << endl << flush;
	}

	void dataset_set_zoom(const string &zstr)
	{
		float z = std::stof(zstr);
		loadedDataset->setZoomRatio(z);
	}

	string createMapFilename ()
	{
		string mapResName;

		if (slDatasourceType==OXFORD_DATASET_TYPE) {
			OxfordDataset::Ptr oxfDs = static_pointer_cast<OxfordDataset> (loadedDataset);
			mapResName = oxfDs->getPath() + "/vmml.map";
		}
		else if(slDatasourceType==MEIDAI_DATASET_TYPE) {
			MeidaiBagDataset::Ptr mdiDs = static_pointer_cast<MeidaiBagDataset> (loadedDataset);
			mapResName = mdiDs->getPath() + ".map";
		}
		return mapResName;
	}


	void map_create_cmd (const stringTokens &cmd)
	{
		ptime ptstart, ptstop;
		double start, stop, duration;

		MapBuilder2 mapBuilder;
		mapBuilder.setMask(loadedDataset->getMask());

		if (slDatasourceType == OXFORD_DATASET_TYPE) {
			mapBuilder.getMap()->setInfo("sourceType", "Oxford");
			mapBuilder.getMap()->setInfo("originalPath", oxfordDsPtr->getPath());
		}

		else if (slDatasourceType == MEIDAI_DATASET_TYPE) {
			meidaiDsPtr->addCameraParameter(meidaiCamera1Params);
			mapBuilder.getMap()->setInfo("sourceType", "Meidai");
			mapBuilder.getMap()->setInfo("originalPath", meidaiDsPtr->getPath());
		}

		if (cmd.size() >= 2) {
			start = stod(cmd[0]);
			stop = stod(cmd[1]);
			duration = stop - start;

			loadedDataset->convertStartDurationToTime(start, duration, ptstart, ptstop);
		}
		else {
			ptstart = loadedDataset->get(0)->getTimestamp();
			ptstop = loadedDataset->last()->getTimestamp();
		}

		int numOfFrames = loadedDataset->size(ptstart, ptstop);
		debug ("About to run mapping with duration "+to_string(duration) +" seconds, " +to_string(numOfFrames) + " frames");

		// build map here
		mapBuilder.addCameraParam(loadedDataset->getCameraParameter());

		Viewer *imgViewer = new Viewer (loadedDataset);
		imgViewer->setMap(mapBuilder.getMap());
		dataItemId currentItemId;

		MapBuilder2::frameCallback frmCallback =
		[&] (const InputFrame &f)
		{
			imgViewer->update(f.sourceId, mapBuilder.getCurrentKeyFrameId());
			cout << f.sourceId << " / " << loadedDataset->size() << endl;
		};
		mapBuilder.registerFrameCallback(frmCallback);

		mapBuilder.runFromDataset(loadedDataset, ptstart, ptstop);

		delete(imgViewer);
		// Stop here

		const string mapFilePath = createMapFilename();
		mapBuilder.getMap()->save(mapFilePath);

		debug ("Mapping done");
		debug ("Dataset time elapsed: " + to_string(duration) + " seconds");
		debug ("Path: " + mapFilePath);
	}


	void mask_set_cmd(const string &maskpath)
	{
		mask = cv::imread(maskpath, cv::IMREAD_GRAYSCALE);
		if (mask.empty()) {
			debug("Unable to fetch mask image file");
			return;
		}
		else
			debug("Mask read; size is "+to_string(mask.cols)+'x'+to_string(mask.rows));

		if (loadedDataset) {
			cv::Mat localizerMask;
			float zr = loadedDataset->getZoomRatio();
			cv::resize(mask, localizerMask, cv::Size(), zr, zr, cv::INTER_CUBIC);
		}
	}


	void map_dump_images()
	{
		if (localizer==NULL) {
			debug ("Map not loaded");
			return;
		}

		MeidaiBagDataset::Ptr meidaiDs;
		auto originalPath = mapSrc->getInfo("originalPath");
		try {
			meidaiDs = MeidaiBagDataset::load(originalPath);
		} catch (exception &e) {
			debug ("Unable to open dataset "+originalPath);
			return;
		}

		boost::filesystem::path dumpDir = mapPath.parent_path() /= "/mapdump";
		boost::filesystem::create_directory(dumpDir);

		for (auto &kfI: mapSrc->allKeyFrames()) {
			auto srcItemId = mapSrc->keyframe(kfI)->getSourceItemId();
			auto srcDataItem = meidaiDs->get(srcItemId);
			cv::Mat cImage = srcDataItem->getImage();
			string imgNew = dumpDir.string() + '/' + (to_string(srcItemId) + ".jpg");
			cv::imwrite(imgNew, cImage);
		}

		debug ("Map keyframes dumped to " + dumpDir.string());
	}

};





int main (int argc, char *argv[])
{
	if (argc > 1) {
		VmmlCliApp::fromScript(argc, argv);
	}

	else {
		VmmlCliApp mainApp(argc, argv);
		mainApp.loop();
	}

	return 0;
}