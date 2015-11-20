#include <taskSequences/sequences/TaskOptimization.h>
#include <ocraWbiPlugins/ocraWbiModel.h>

#ifndef ERROR_THRESH
#define ERROR_THRESH 0.03 // Goal error threshold for hand tasks
#endif

#ifndef VAR_THRESH
#define VAR_THRESH 0.99
#endif

#ifndef TIME_LIMIT
#define TIME_LIMIT 15.0 // Maximum time to be spent on any trajectory.
#endif

TaskOptimization::TaskOptimization()
{
    connectToSolverPorts();
}

TaskOptimization::~TaskOptimization()
{
    optVarsPortOut.close();
    costPortOut.close();
    optVarsPortIn.close();

    l_hand_port.close();
    l_hand_target_port.close();
    r_hand_port.close();
    r_hand_target_port.close();
}

void TaskOptimization::connectToSolverPorts()
{
    optVarsPortOut_name = "/opt/task/vars:o";
    costPortOut_name = "/opt/task/cost:o";
    optVarsPortIn_name = "/opt/task/vars:i";

    optVarsPortOut.open(optVarsPortOut_name.c_str());
    costPortOut.open(costPortOut_name.c_str());
    optVarsPortIn.open(optVarsPortIn_name.c_str());

    double connectionElapsedTime = 0.0;
    double waitInterval = 2.0;
    double connectionTimeOut = 20.0;

    while (!yarp.connect(optVarsPortOut_name.c_str(), "/opt/solver/vars:i") && connectionElapsedTime <= connectionTimeOut ){
        std::cout << "Waiting to connect to solver ports. Please make sure the taskOptimizer module is running." << std::endl;
        yarp::os::Time::delay(waitInterval);
        connectionElapsedTime += waitInterval;
    }
    connectionElapsedTime = 0.0;
    while (!yarp.connect(costPortOut_name.c_str(), "/opt/solver/cost:i") && connectionElapsedTime <= connectionTimeOut ){
        std::cout << "Waiting to connect to solver ports. Please make sure the taskOptimizer module is running." << std::endl;
        yarp::os::Time::delay(waitInterval);
        connectionElapsedTime += waitInterval;
    }
    connectionElapsedTime = 0.0;
    while (!yarp.connect(optVarsPortIn_name.c_str(), "/opt/solver/vars:o") && connectionElapsedTime <= connectionTimeOut ){
        std::cout << "Waiting to connect to solver ports. Please make sure the taskOptimizer module is running." << std::endl;
        yarp::os::Time::delay(waitInterval);
        connectionElapsedTime += waitInterval;
    }
}

void TaskOptimization::doInit(wocra::wOcraController& ctrl, wocra::wOcraModel& model)
{

    ocraWbiModel& wbiModel = dynamic_cast<ocraWbiModel&>(model);

    varianceThresh = Eigen::Array3d::Constant(VAR_THRESH);

    /*
    *   Task coefficients
    */
    bool usesYARP = true;
    //  fullPosture
    double Kp_fullPosture       = 20.0;
    double Kd_fullPosture       = 2.0 * sqrt(Kp_fullPosture);
    double weight_fullPosture   = 0.0001;

    //  torsoPosture
    double Kp_torsoPosture      = 20.0;
    double Kd_torsoPosture      = 2.0 * sqrt(Kp_torsoPosture);
    double weight_torsoPosture  = 0.01;

    //  leftHand
    double Kp_leftHand = 60.0;
    double Kd_leftHand = 2.0 *sqrt(Kp_leftHand);
    Eigen::Vector3d weights_leftHand = Eigen::Vector3d::Ones(3);

    //  rightHand
    double Kp_rightHand = 60.0;
    double Kd_rightHand = 2.0 *sqrt(Kp_rightHand);
    Eigen::Vector3d weights_rightHand = Eigen::Vector3d::Ones(3);


    /*
    *   Task constructors
    */

    // fullPosture
    Eigen::VectorXd nominal_q = Eigen::VectorXd::Zero(model.nbInternalDofs());
    getHomePosture(model, nominal_q);

    taskManagers["fullPosture"] = new wocra::wOcraFullPostureTaskManager(ctrl, model, "fullPosture", ocra::FullState::INTERNAL, Kp_fullPosture, Kd_fullPosture, weight_fullPosture, nominal_q, usesYARP);


    // torsoPosture
    Eigen::VectorXi torso_indices(3);
    Eigen::VectorXd torsoTaskPosDes(3);
    torso_indices << wbiModel.getDofIndex("torso_pitch"), wbiModel.getDofIndex("torso_roll"), wbiModel.getDofIndex("torso_yaw");
    torsoTaskPosDes << 0.0, 0.0, 0.0;

    taskManagers["torsoPosture"] = new wocra::wOcraPartialPostureTaskManager(ctrl, model, "torsoPosture", ocra::FullState::INTERNAL, torso_indices, Kp_torsoPosture, Kd_torsoPosture, weight_torsoPosture, torsoTaskPosDes, usesYARP);


    //  leftHand
    Eigen::Vector3d l_handDisp(0.05, 0.0, 0.0); // Moves the task frame to the center of the hand.
    taskManagers["leftHand"] = new wocra::wOcraVariableWeightsTaskManager(ctrl, model, "leftHand", "l_hand", l_handDisp, Kp_leftHand, Kd_leftHand, weights_leftHand, usesYARP);

    //  rightHand
    Eigen::Vector3d r_handDisp(0.05, 0.0, 0.0); // Moves the task frame to the center of the hand.
    taskManagers["rightHand"] = new wocra::wOcraVariableWeightsTaskManager(ctrl, model, "rightHand", "r_hand", r_handDisp, Kp_rightHand, Kd_rightHand, weights_rightHand, usesYARP);


    /*
    *   Trajectory constructors
    */

    leftHandTrajectory = new wocra::wOcraGaussianProcessTrajectory();
    rightHandTrajectory = new wocra::wOcraGaussianProcessTrajectory();

    // leftHandTrajectory->setWaypoints(startingPos, desiredPos)

    /*
    *   Cast tasks to derived classes to access their virtual functions
    */
    leftHandTask = dynamic_cast<wocra::wOcraVariableWeightsTaskManager*>(taskManagers["leftHand"]);

    rightHandTask = dynamic_cast<wocra::wOcraVariableWeightsTaskManager*>(taskManagers["rightHand"]);


    /*
    *   Variables used in the doUpdate control logic
    */
    lHandIndex = model.getSegmentIndex("l_hand");
    rHandIndex = model.getSegmentIndex("r_hand");


    initTrigger = true;

    std::string l_hand_port_name = "/lHandFrame:o";
    l_hand_port.open(l_hand_port_name.c_str());
    yarp.connect(l_hand_port_name.c_str(), "/leftHandSphere:i");

    std::string l_hand_target_port_name = "/lHandTarget:o";
    l_hand_target_port.open(l_hand_target_port_name.c_str());
    yarp.connect(l_hand_target_port_name.c_str(), "/leftHandTargetSphere:i");

    std::string r_hand_port_name = "/rHandFrame:o";
    r_hand_port.open(r_hand_port_name.c_str());
    yarp.connect(r_hand_port_name.c_str(), "/rightHandSphere:i");

    std::string r_hand_target_port_name = "/rHandTarget:o";
    r_hand_target_port.open(r_hand_target_port_name.c_str());
    yarp.connect(r_hand_target_port_name.c_str(), "/rightHandTargetSphere:i");

    //Figure out waypoints
    rHandPosStart = model.getSegmentPosition(model.getSegmentIndex("r_hand")).getTranslation();

    int dofIndex = 0;
    Eigen::Vector3d rHandDisplacement = Eigen::Vector3d::Zero();
    rHandDisplacement(dofIndex) = 0.3; // meters
    rHandPosEnd = rHandPosStart + rHandDisplacement;

    std::cout << "\n\n\n rHandPosStart = "<<rHandPosStart.transpose() << "\n rHandPosEnd = " << rHandPosEnd.transpose() << "  \n\n\n" << std::endl;

    rightHandTrajectory->setWaypoints(rHandPosStart, rHandPosEnd);


    std::vector<Eigen::VectorXi> dofToOptimize(1);
    dofToOptimize[0].resize(2);
    dofToOptimize[0] << 0, dofIndex+1;
    optVariables = rightHandTrajectory->getBoptVariables(1, dofToOptimize);

    std::cout << "optVariables\n" << optVariables << std::endl;

    yarp::os::Bottle& b = optVarsPortOut.prepare();
    bottleEigenVector(b, optVariables);
    optVarsPortOut.write();
}



void TaskOptimization::doUpdate(double time, wocra::wOcraModel& state, void** args)
{
    Eigen::Vector3d currentLeftHandPos = (leftHandTask->getTaskFramePosition() + Eigen::Vector3d(0,0,1)).array() * Eigen::Array3d(-1, -1, 1);
    Eigen::Vector3d currentRightHandPos = (rightHandTask->getTaskFramePosition() + Eigen::Vector3d(0,0,1)).array() * Eigen::Array3d(-1, -1, 1);


    yarp::os::Bottle l_hand_output, r_hand_output;
    bottleEigenVector(l_hand_output, currentLeftHandPos);
    bottleEigenVector(r_hand_output, currentRightHandPos);
    l_hand_port.write(l_hand_output);
    r_hand_port.write(r_hand_output);


    if (initTrigger) {

    }


    if ( (abs(time - resetTimeLeft) >= TIME_LIMIT) || (attainedGoal(state, lHandIndex)) )
    {
        resetTimeLeft = time;
    }
    else
    {

        leftHandTrajectory->getDesiredValues(time, desiredPosVelAcc_leftHand, desiredVariance_leftHand);
        desiredWeights_leftHand = mapVarianceToWeights(desiredVariance_leftHand);

        leftHandTask->setState(desiredPosVelAcc_leftHand.col(0));
        leftHandTask->setWeights(desiredWeights_leftHand);

    }

    if ( (abs(time - resetTimeRight) >= TIME_LIMIT) || (attainedGoal(state, rHandIndex)) )
    {
        resetTimeRight = time;

    }
    else
    {
        rightHandTrajectory->getDesiredValues(time, desiredPosVelAcc_rightHand, desiredVariance_rightHand);
        desiredWeights_rightHand = mapVarianceToWeights(desiredVariance_rightHand);

        rightHandTask->setState(desiredPosVelAcc_rightHand.col(0));
        rightHandTask->setWeights(desiredWeights_rightHand);

    }

    Eigen::Vector3d currentDesiredPosition_leftHand_transformed = (currentDesiredPosition_leftHand + Eigen::Vector3d(0,0,1)).array() * Eigen::Array3d(-1,-1,1);
    Eigen::Vector3d currentDesiredPosition_rightHand_transformed = (currentDesiredPosition_rightHand + Eigen::Vector3d(0,0,1)).array() * Eigen::Array3d(-1,-1,1);

    yarp::os::Bottle l_hand_target_output, r_hand_target_output;
    bottleEigenVector(l_hand_target_output, currentDesiredPosition_leftHand_transformed);
    bottleEigenVector(r_hand_target_output, currentDesiredPosition_rightHand_transformed);
    l_hand_target_port.write(l_hand_target_output);
    r_hand_target_port.write(r_hand_target_output);


    // if(isCartesion){std::cout << "\nFinal desired position: " << desiredPos.transpose() << std::endl;}
    // std::cout << "\nDesired position: " << desiredPosVelAcc.col(0).transpose() << std::endl;
    // std::cout << "Current position: " << state.getSegmentPosition(lHandIndex).getTranslation().transpose()<< std::endl;
    // std::cout << "Error: " << tmp_tmLeftHandCart->getTaskError().transpose() << "   norm: " << tmp_tmLeftHandCart->getTaskErrorNorm() << std::endl;




}


void TaskOptimization::bottleEigenVector(yarp::os::Bottle& bottle, const Eigen::VectorXd& vecToBottle, const bool encapsulate)
{
    bottle.clear();
    for(int i =0; i<vecToBottle.size(); i++){
        bottle.addDouble(vecToBottle(i));
    }
}

// void encapsulateBottleData()


bool TaskOptimization::attainedGoal(wocra::wOcraModel& state, int segmentIndex)
{
    double error;
    Eigen::Vector3d currentDesiredPosition, taskFrame;
    if (segmentIndex==lHandIndex) {
        currentDesiredPosition = currentDesiredPosition_leftHand;
        taskFrame = leftHandTask->getTaskFramePosition();
    }
    else if (segmentIndex==rHandIndex) {
        currentDesiredPosition = currentDesiredPosition_rightHand;
        taskFrame = rightHandTask->getTaskFramePosition();

    }
    else{
        std::cout << "[ERROR] TaskOptimization::attainedGoal - segment name doesn't match either l_hand or r_hand" << std::endl;
        return false;
    }

    error = (currentDesiredPosition - taskFrame ).norm();
    bool result = error <= ERROR_THRESH;
    return result;
}


Eigen::VectorXd TaskOptimization::mapVarianceToWeights(Eigen::VectorXd& variance)
{
    double beta = 1.0;
    variance /= maxVariance;
    variance = variance.array().min(varianceThresh); //limit variance to 0.99 maximum
    Eigen::VectorXd weights = (Eigen::VectorXd::Ones(variance.rows()) - variance) / beta;
    return weights;
}