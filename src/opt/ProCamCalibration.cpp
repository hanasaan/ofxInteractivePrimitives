#include "ProCamCalibration.h"

#include "Poco/Util/XMLConfiguration.h"

#include "ofxCv.h"

PROJECTOR_CALIBRATION_BEGIN_NAMESPACE

static ofMatrix4x4 homography2glModelViewMatrix(const cv::Mat& homography);

#pragma mark - CameraParam

bool CameraParam::load(const string& path)
{
	ofFile file(path);
	if (!file.exists()) return false;

	string tmp;

	file >> tmp;
	file >> projection;
	file >> tmp;
	file >> modelview;

	return true;
}

void CameraParam::save(const string& path)
{
	ofFile file(path, ofFile::WriteOnly);

	file << "#projection" << endl;
	file << projection;
	file << endl;

	file << "#modelview" << endl;
	file << modelview;
	file << endl;

	file.close();
}

#pragma mark - CalibrationMarker

void Marker::draw()
{
	ofxInteractivePrimitives::Marker::draw();

	if (hasFocus())
	{
		ofNoFill();
		ofRect(-15, -15, 30, 30);
	}
    
    if (getObjectPoint().lengthSquared() < FLT_EPSILON && ofGetFrameNum() % 30  > 15) {
        ofNoFill();
        ofSetLineWidth(3);
        ofSetColor(255, 0, 0);
        ofRect(-15, -15, 30, 30);
    }
}

void Marker::update()
{
	stringstream ss;
	if (!marker_label.empty()) ss << marker_label << endl;
	ss << getX() << ":" << getY() << endl;
	ss << (int)object_pos.x << ":" << (int)object_pos.y << ":"
	   << (int)object_pos.z;

	text = ss.str();

	if (last_position != getPosition())
	{
		last_position = getPosition();
		need_update_calib = true;
	}
}

void Marker::keyPressed(int key)
{
	if (key == OF_KEY_LEFT)
		move(-1, 0, 0);
	else if (key == OF_KEY_RIGHT)
		move(1, 0, 0);
	else if (key == OF_KEY_UP)
		move(0, -1, 0);
	else if (key == OF_KEY_DOWN)
		move(0, 1, 0);
}

#pragma mark - Manager

void Manager::setup(size_t num_markers)
{
	clearChildren();

	markers.resize(num_markers);

	for (int i = 0; i < num_markers; i++)
	{
		markers[i] = Marker::Ref(new Marker(*this));
	}
}

ofMatrix4x4 Manager::getHomography()
{
	assert(markers.size() >= 4);

	markUpdated();

	using namespace cv;
	using namespace ofxCv;

	vector<Point2f> srcPoints, dstPoints;
	for (int i = 0; i < markers.size(); i++)
	{
		Marker* o = markers[i].get();

		dstPoints.push_back(Point2f(o->getX(), o->getY()));
		srcPoints.push_back(Point2f(o->object_pos.x, o->object_pos.y));
	}

	Mat homography = findHomography(Mat(srcPoints), Mat(dstPoints));
	return homography2glModelViewMatrix(homography);
}

void Manager::draw()
{
	ofPushStyle();

	if (getFocusObject())
	{
		ofPushStyle();

		ofSetLineWidth(3);

		ofSetColor(255, 0, 0);

		ofVec2f p = getFocusObject()->getPosition();
		ofNoFill();
		ofDrawCircle(toGlm(p), 40);

		ofDrawCircle(toGlm(p), 10);

		ofLine(-10000, p.y, 10000, p.y);
		ofLine(p.x, -10000, p.x, 10000);

		ofPopStyle();
	}

	RootNode::draw();

	ofPopStyle();
}

void Manager::setSelectedImagePoint(int x, int y)
{
	Marker* m = (Marker*)getFocusObject();
	if (m) m->object_pos.set(x, y, 0);
}

Marker* Manager::getSelectedMarker() { return (Marker*)getFocusObject(); }

bool Manager::getNeedUpdateCalibration() const
{
	for (int i = 0; i < markers.size(); i++)
		if (markers[i]->need_update_calib) return true;

	return false;
}

void Manager::markUpdated()
{
	for (int i = 0; i < markers.size(); i++)
	{
		if (markers[i]->need_update_calib)
			markers[i]->need_update_calib = false;
	}
}

float Manager::getEstimatedCameraPose(cv::Size image_size,
									  cv::Mat& camera_matrix, cv::Mat& rvec,
									  cv::Mat& tvec, float force_fov, ofVec2f lens_offset_pix)
{
	if (markers.size() <= 6) return -1;

	markUpdated();

	using namespace ofxCv;
	using namespace cv;

	vector<cv::Point3f> object_points;
	vector<cv::Point2f> image_points;

	for (int i = 0; i < markers.size(); i++)
	{
		Marker* o = markers[i].get();
		object_points.push_back(toCv(o->object_pos));
		image_points.push_back(toCv((ofVec2f)o->getPosition()));
	}

	cv::Mat dist_coeffs = cv::Mat::zeros(8, 1, CV_64F);

	float fov = 60;
	if (force_fov != 0) fov = force_fov;

	float f = (image_size.height / 2) * tan(ofDegToRad(fov / 2.0));
	camera_matrix = (cv::Mat_<double>(3, 3) << f, 0, image_size.width / 2 + lens_offset_pix.x, 0, f,
					 image_size.height / 2 + lens_offset_pix.y, 0, 0, 1);

	float rms = 0;

	if (force_fov == 0)
	{
		vector<cv::Mat> rvecs;
		vector<cv::Mat> tvecs;
		
		vector<vector<cv::Point3f> > object_points_arr(1);
		vector<vector<cv::Point2f> > image_points_arr(1);
		
		object_points_arr[0] = object_points;
		image_points_arr[0] = image_points;

		int flags = 0;
		flags |= CV_CALIB_USE_INTRINSIC_GUESS;

		flags |= CV_CALIB_FIX_ASPECT_RATIO;
		flags |= CV_CALIB_ZERO_TANGENT_DIST;
		flags |= (CV_CALIB_FIX_K1 | CV_CALIB_FIX_K2 | CV_CALIB_FIX_K3 |
				  CV_CALIB_FIX_K4 | CV_CALIB_FIX_K5 | CV_CALIB_FIX_K6 |
				  CV_CALIB_RATIONAL_MODEL);

        if (lens_offset_pix.lengthSquared() > FLT_EPSILON) {
             flags |= CV_CALIB_FIX_PRINCIPAL_POINT;
        }

		rms = cv::calibrateCamera(object_points_arr, image_points_arr, image_size,
								  camera_matrix, dist_coeffs, rvecs, tvecs,
								  flags);
		
		rvec = rvecs[0];
		tvec = tvecs[0];
	}
	else
	{
		cv::Mat rvecs;
		cv::Mat tvecs;

		cv::solvePnP(object_points, image_points, camera_matrix, dist_coeffs, rvecs, tvecs);
		rms = 1;
		
		rvec = rvecs;
		tvec = tvecs;
	}


	return rms;
}

Marker::Ref Manager::addMarker(const string& marker_label)
{
	Marker* o = new Marker(marker_label, *this);
	Marker::Ref ref = Marker::Ref(o);
	markers.push_back(ref);
	return ref;
}

void Manager::removeMarker(Marker::Ref o)
{
	vector<Marker::Ref>::iterator it = markers.begin();
	while (it != markers.end())
	{
		if (o == *it)
		{
			(*it)->dispose();
			it = markers.erase(it);
		}
		else
			it++;
	}
}

void Manager::clear()
{
	clearChildren();
	markers.clear();
}

float Manager::getEstimatedCameraPose(int width, int height, CameraParam& param,
									  float near_dist, float far_dist, float force_fov, ofVec2f lens_offset_pix)
{
	cv::Mat camera_matrix, rvec, tvec;
	cv::Size image_size(width, height);

	float rms = getEstimatedCameraPose(image_size, camera_matrix, rvec, tvec,
									   force_fov, lens_offset_pix);

	if (rms > 0)
	{
		param =
			CameraParam(width, height, camera_matrix, rvec, tvec, near_dist, far_dist);
	}

	return rms;
}

// IO

bool Manager::load(const string& path)
{
	if (!ofFile::doesFileExist(path)) return false;

	Poco::AutoPtr<Poco::Util::XMLConfiguration> config =
		new Poco::Util::XMLConfiguration;
	config->loadEmpty("markers");
	config->load(ofToDataPath(path));

	vector<string> keys;
	config->keys(keys);

	clear();

	for (int i = 0; i < keys.size(); i++)
	{
		Marker::Ref o = addMarker();
		string m = keys[i];

		o->setPosition(config->getDouble(m + ".image[@x]", 0),
					   config->getDouble(m + ".image[@y]", 0), 0);

		o->object_pos.set(config->getDouble(m + ".object[@x]", 0),
						  config->getDouble(m + ".object[@y]", 0),
						  config->getDouble(m + ".object[@z]", 0));
		
		o->setLabel(config->getString(m + ".label", ""));
        
        o->update();
        o->need_update_calib = false;
	}
	
	return true;
}

void Manager::save(string path)
{
	Poco::AutoPtr<Poco::Util::XMLConfiguration> config =
		new Poco::Util::XMLConfiguration;
	config->loadEmpty("markers");

	for (int i = 0; i < markers.size(); i++)
	{
		Marker::Ref o = (*this)[i];

		string m = "marker[" + ofToString(i) + "]";

		config->setDouble(m + ".image[@x]", o->getX());
		config->setDouble(m + ".image[@y]", o->getY());

		config->setDouble(m + ".object[@x]", o->object_pos.x);
		config->setDouble(m + ".object[@y]", o->object_pos.y);
		config->setDouble(m + ".object[@z]", o->object_pos.z);
		
		config->setString(m + ".label", o->getLabel());
	}

	config->save(ofToDataPath(path));
}

// ====

static ofMatrix4x4 homography2glModelViewMatrix(const cv::Mat& homography)
{
	ofMatrix4x4 matrix;

	matrix(0, 0) = homography.at<double>(0, 0);
	matrix(0, 1) = homography.at<double>(1, 0);
	matrix(0, 2) = 0;
	matrix(0, 3) = homography.at<double>(2, 0);

	matrix(1, 0) = homography.at<double>(0, 1);
	matrix(1, 1) = homography.at<double>(1, 1);
	matrix(1, 2) = 0;
	matrix(1, 3) = homography.at<double>(2, 1);

	matrix(2, 0) = 0;
	matrix(2, 1) = 0;
	matrix(2, 2) = 1;
	matrix(2, 3) = 0;

	matrix(3, 0) = homography.at<double>(0, 2);
	matrix(3, 1) = homography.at<double>(1, 2);
	matrix(3, 2) = 0;
	matrix(3, 3) = 1;

	return matrix;
}

PROJECTOR_CALIBRATION_END_NAMESPACE
