var notgios = angular.module('notgios', ['ngRoute', 'ngCookies']);

notgios.config(['$routeProvider', '$locationProvider', function ($routeProvider, $locationProvider) {
  $routeProvider.when('/', {
    templateUrl: '/templates/home.html',
    controller: 'homeController'
  }).when('/tasks', {
    templateUrl: '/templates/tasks.html',
    controller: 'taskController'
  }).when('/alarms', {
    templateUrl: '/templates/alarms.html',
    controller: 'alarmController'
  }).when('/contacts', {
    templateUrl: '/templates/contacts.html',
    controller: 'contactController'
  }).otherwise({
    redirectTo: '/'
  });

  $locationProvider.html5Mode(true);
}]);

notgios.factory('authenticated', ['$cookies', '$http', function ($cookies, $http) {
  var authentication = {};

  authentication.loggedIn = function () {
    return $cookies.get('token') != null;
  };

  authentication.getData = function (url, success, failure) {
    $http({
      method: 'GET',
      url: url,
      headers: {
        'Authorization': 'Bearer ' + $cookies.get('token')
      }
    }).then(success, failure);
  };

  authentication.sendData = function (url, data, success, failure) {
    $http({
      method: 'POST',
      url: url,
      headers: {
        'Authorization': 'Bearer ' + $cookies.get('token')
      }
    }).then(success, failure);
  };

  authentication.logIn = function (token) {
    if (token) {
      $cookies.put('token', token);
    }
  };

  authentication.logOut = function () {
    $cookies.remove('token');
  };
  
  return authentication;
}]);

notgios.controller('homeController', ['$scope', '$interval', 'authenticated', function ($scope, $interval, authenticated) {

  $scope.loggedIn = authenticated.loggedIn;
  $scope.serverData = {};

  $scope.dataInterval = $interval(function dataInterval() {
    authenticated.getData('get_servers', function success(response) {
      for (var i = 0; i < response.data.connectedServers.length; i++) {
        server = response.data.connectedServers[i];
        server.lastSeen = new Date(server.lastSeen);
      }
      for (var i = 0; i < response.data.disconnectedServers.length; i++) {
        server = response.data.disconnectedServers[i];
        server.lastSeen = new Date(server.lastSeen);
      }
      $scope.serverData = response.data;
    }, function failure(response) {
      $scope.refreshMessage = 'Currently unable to contact the server, please check your connection.';
    });
  }, 1000);

  $scope.$on('$destroy', function destruct() {
    $interval.cancel($scope.dataInterval);
  });

  $scope.showServer = function (server) {
    $scope.shownServer = server;
  };

}]);

notgios.controller('taskController', ['$scope', '$interval', 'authenticated', function  ($scope, $interval, authenticated) {

  $scope.loggedIn = authenticated.loggedIn;
  $scope.taskData = null;
  $scope.metrics = [];

  $scope.dataInterval = $interval(function dataInterval() {
    authenticated.getData('get_tasks', function success(response) {
      $scope.taskData = response.data;
    }, function failure(response) {
      $scope.refreshMessage = 'Currently unable to contact the server, please check your connection.';
    });

    if ($scope.taskData && $scope.taskData.length > 0) {
      for (var i = 0; i < $scope.taskData.length; i++) {
        (function (num) {
          authenticated.getData('get_metrics/' + num, function success(response) {
            $scope.metrics[num] = response.data;
          }, function failure(response) {
            $scope.refreshMessage = 'Currently unable to contact the server, please check your connection.';
          });
        })(i);
      }
    }
  }, 1000);

  $scope.$on('$destroy', function destruct() {
    $interval.cancel($scope.dataInterval);
  });

  $scope.showVis = function (task) {
    $scope.shownVis = task;
    $('#highcharts').highcharts();
  };

}]);

notgios.controller('alarmController', ['$scope', '$http', function ($scope, $http) {

}]);

notgios.controller('contactController', ['$scope', '$http', function ($scope, $http) {

}]);

notgios.controller('navbarController', ['$scope', '$http', '$route', 'authenticated', function ($scope, $http, $route, authenticated) {

  $scope.loggedIn = authenticated.loggedIn;

  $scope.isActive = function (path) {
    if ($route.current && $route.current.regexp) return $route.current.regexp.test(path);
    return false;
  }

  $scope.login = function ($event) {
    $event.stopPropagation();
    if ($scope.user && $scope.pass) {
      $http({
        method: 'POST',
        url: 'sign_in',
        data: {
          username: $scope.user,
          password: $scope.pass
        }
      }).then(function success(response) {
        $scope.submissionError = '';
        authenticated.logIn(response.data);
      }, function failure(response) {
        if (response.status == 400) $scope.loginError = 'User does not exist.'
        else $scope.loginError = 'Password is incorrect';
      });
    } else {
      $scope.loginError = 'Please enter both a username and a password.';
    }
  };

  $scope.logout = function () {
    authenticated.logOut();
  };

}]);

notgios.controller('signupController', ['$scope', '$http', 'authenticated', function ($scope, $http, authenticated) {

  $scope.dropdown = 'Phone Number';

  $scope.loggedIn = authenticated.loggedIn;

  $scope.setDropdown = function (value) {
    $scope.dropdown = value;
    if (value == 'Phone Number') $scope.contactType = 'number';
    else $scope.contactType = 'email';
  }
  
  // I should have Angular do the form validation for me, but I don't have time to figure it out right now.
  $scope.signUp = function ($event) {
    if ($scope.username && $scope.username.length > 0 && $scope.password && $scope.password.length > 0) {
      if ($scope.password == $scope.passConfirm && $scope.contact && $scope.contact.length > 0) {
        $http({
          method: 'POST',
          url: '/sign_up',
          data: {
            username: $scope.username,
            password: $scope.password,
            contact: $scope.contact
          }
        }).then(function success(response) {
          $scope.submissionError = '';
          authenticated.logIn(response.data);
        }, function failure(response) {
          $event.stopPropagation();
          if (response.status == 400) $scope.submissionError = 'A user with that name already exists. Please pick another';
          else $scope.submissionError = 'There was an error during submission. Please check your internet and try again.';
        });
      } else {
        $event.stopPropagation();
        $scope.submissionError = 'The form did not pass validation. Please check your entries and try again.'
      }
    } else {
      $event.stopPropagation();
      $scope.submissionError = 'The form did not pass validation. Please check your entries and try again.'
    }
  };

}]);

notgios.directive('serverTable', function () {
  return {
    templateUrl: '/templates/server_table.html',
    replace: true,
    scope: {
      servers: "=servers",
      status: "=connected",
      showServer: "=callback",
      title: "=header"
    }
  };
});

notgios.directive('taskTable', function () {
  return {
    templateUrl: '/templates/task_table.html',
    replace: true,
    scope: {
      taskData: "=tasks",
      showVis: "=callback"
    }
  };
});
