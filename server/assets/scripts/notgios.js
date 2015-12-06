var notgios = angular.module('notgios', ['ngRoute', 'ngCookies', 'initialValue']);

notgios.config(['$routeProvider', '$locationProvider', function ($routeProvider, $locationProvider) {
  $routeProvider.when('/', {
    templateUrl: '/templates/home.html',
    controller: 'homeController'
  }).when('/tasks', {
    templateUrl: '/templates/tasks.html',
  controller: 'taskController'
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
    if ($cookies.get('token') != null) {
      $http({
        method: 'GET',
        url: url
      }).then(success, failure);
    }
  };

  authentication.sendData = function (url, data, success, failure) {
    if ($cookies.get('token') != null) {
      $http({
        method: 'POST',
        url: url,
        data: data
      }).then(success, failure);
    }
  };

  authentication.logIn = function (token) {
    if (token) {
      var tomorrow = new Date();
      tomorrow.setDate(tomorrow.getDate() + 1);
      $cookies.put('token', token, { expires: tomorrow });
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
  $scope.server = {};

  $scope.dataInterval = $interval(function dataInterval() {
    authenticated.getData('get_servers', function success(response) {
      for (var i = 0; i < response.data.connectedServers.length; i++) {
        var server = response.data.connectedServers[i];
        if (server.lastSeen) server.lastSeen = new Date(server.lastSeen * 1000);
        else server.lastSeen = 'Never';
      }
      for (var i = 0; i < response.data.disconnectedServers.length; i++) {
        var server = response.data.disconnectedServers[i];
        if (server.lastSeen) server.lastSeen = new Date(server.lastSeen * 1000);
        else server.lastSeen = 'Never';
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
    $scope.server.serverName = server.name;
    $scope.server.serverAddress = server.address;
    $scope.server.sshPort = server.sshPort;
  };

  $scope.saveServer = function ($event) {
    $event.stopPropagation();
    var url;
    if ($scope.shownServer) url = 'update_server';
    else url = 'add_server';
    authenticated.sendData(url, $scope.server, function success(response) {
      $('#server-modal').modal('hide');
      $scope.shownServer = null;
    }, function failure(response) {
      $scope.modalMessage = 'There was an error saving the server, please check your internet connection.';
    });
  };

  $scope.deleteServer = function (server) {
    authenticated.sendData('delete_server', server, function success(response) {
      $('#server-modal').modal('hide');
      $scope.shownServer = null;
      $scope.server = {};
    }, function failure(response) {
      $scope.modalMessage = 'There was an error saving the server, please check your internet connection.';
    });
  };

}]);

notgios.controller('taskController', ['$scope', '$interval', 'authenticated', function  ($scope, $interval, authenticated) {

  $scope.loggedIn = authenticated.loggedIn;
  $scope.metrics = [];
  $scope.metricCount = 100;

  $scope.dataInterval = $interval(function dataInterval() {
    authenticated.getData('get_tasks', function success(response) {
      $scope.taskData = response.data;
    }, function failure(response) {
      $scope.refreshMessage = 'Currently unable to contact the server, please check your connection.';
    });
  }, 1000);

  $scope.updateMetrics = function () {
    authenticated.getData('get_metrics/' + $scope.shownVis.id + '?count=' + $scope.metricCount, function success(response) {
      var found = -1;
      for (var i = 0; i < response.data.length; i++) {
        var metric = response.data[i];
        if ($scope.metrics[0] && $scope.metrics[0].timestamp == metric.timestamp) {
          found = i;
          break;
        }
      }
      if (found != 0) {
        var series;
        if ((series = $('#highcharts').highcharts().series[0]) == undefined) {
          series = $('#highcharts').highcharts().addSeries('stuff');
        }
        if (found < 0) found = response.data.length;
        var subset = response.data.slice(0, found);
        var key;
        for (var k in subset[0]) {
          if (subset[0].hasOwnProperty(k) && k != 'timestamp') {
            key = k;
            break;
          }
        }
        for (var i = 0; i < subset.length; i++) {
          var point = subset[i];
          series.addPoint([point.timestamp * 1000, parseInt(point[key])]);
        }
      }
      $scope.metrics = response.data;
    }, function failure(response) {
      $scope.metricMessage = 'Current unable to contact the server, please check your connection.';
    });
  };

  $scope.hasOptions = function (task) {
    return !$.isEmptyObject(task.options);
  };

  $scope.$on('$destroy', function destruct() {
    $interval.cancel($scope.dataInterval);
  });

  $scope.showConfig = function (task, server) {
    if (task.type == undefined) {
      task.type = 'process';
      task.options = {};
    }
    $scope.shownTask = task;
    $scope.shownServer = server;
  };

  $scope.saveTask = function () {
    var sanitized = {};
    for (var option in $scope.shownTask.options) {
      if ($scope.shownTask.options.hasOwnProperty(option)) {
        switch ($scope.shownTask.type) {
          case 'process':
            if (option == 'keepalive' || option == 'runcmd' || option == 'pidfile') {
              sanitized[option] = $scope.shownTask.options[option];
            }
            break;
          case 'directory':
            if (option == 'path') sanitized['path'] = $scope.shownTask.options['path']
            break;
        }
      }
    }
    if ($scope.shownTask.type == 'directory') $scope.shownTask.metric = 'memory'
    $scope.shownTask.options = sanitized;
    var data = JSON.parse(JSON.stringify($scope.shownTask));
    data.server = $scope.shownServer.address;

    authenticated.sendData('update_job', data, function success(response) {
      $('#config-modal').modal('hide');
    }, function failure(response) {
      $scope.taskErrorMessage = 'Currently unable to contact the server, please check your connection.';
    });
  }

  $scope.upcase = function (word) {
    if (word == undefined || word == null) return;
    var character = word.charAt(0);
    return character.toUpperCase() + word.substring(1, word.length);
  };

  $scope.showVis = function (task) {
    $scope.shownVis = task;
    $scope.collectMetrics = true;
    var yAxisName, suffix;
    switch (task.type) {
      case 'process', 'total':
        if (task.metric == 'cpu') {
          yAxisName = 'Percentage CPU Used';
          suffix = '% CPU';
        } else {
          yAxisName = 'Bytes Used';
          suffix = ' Bytes';
        }
        break;
      case 'directory':
        yAxisName = 'Bytes Used';
        suffix = ' Bytes';
    }
    $('#highcharts').highcharts({
      chart: {
        spacingTop: 0,
        spacingLeft: 0,
        spacingRight: 0,
        spacingBottom: 0
      },
      title: {
        text: $scope.upcase(task.type) + ' ' + $scope.upcase(task.metric) + ' Usage over time'
      },
      xAxis: {
        type: 'datetime',
        title: {
          text: 'Time Taken'
        }
      },
      yAxis: {
        title: {
          text: yAxisName
        }
      },
      tooltip: {
        valueSuffix: suffix
      },
      series: {
        data: []
      }
    });
    $scope.metricInterval = $interval($scope.updateMetrics, 2000);
  };

  $scope.hideVis = function () {
    $interval.cancel($scope.metricInterval);
    $scope.shownVis = null;
    $scope.metrics.length = 0;
  }

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
  $scope.signUp = function () {
    if ($scope.username && $scope.username.length > 0 && $scope.password && $scope.password.length > 0) {
      if ($scope.password == $scope.passConfirm) {
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
          $('#signup-modal').modal('hide');
          authenticated.logIn(response.data);
        }, function failure(response) {
          if (response.status == 400) $scope.submissionError = 'A user with that name already exists. Please pick another';
          else $scope.submissionError = 'There was an error during submission. Please check your internet and try again.';
        });
      } else {
        $scope.submissionError = 'The form did not pass validation. Please check your entries and try again.'
      }
    } else {
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
